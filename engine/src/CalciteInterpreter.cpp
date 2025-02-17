#include "CalciteInterpreter.h"
#include "CodeTimer.h"
#include "execution_graph/logic_controllers/PhysicalPlanGenerator.h"

using namespace fmt::literals;

std::shared_ptr<ral::cache::graph> generate_graph(std::vector<ral::io::data_loader> input_loaders,
	std::vector<ral::io::Schema> schemas,
	std::vector<std::string> table_names,
	std::vector<std::string> table_scans,
	std::string logicalPlan,
	Context & queryContext,
    const std::string &sql) {

	CodeTimer blazing_timer;
    std::shared_ptr<spdlog::logger> batchLogger = spdlog::get("batch_logger");
    std::shared_ptr<spdlog::logger> queriesLogger = spdlog::get("queries_logger");

	try {
		assert(input_loaders.size() == table_names.size());
		 
		auto tree = std::make_shared<ral::batch::tree_processor>(
			ral::batch::node(),
			queryContext.clone(),
			input_loaders,
			schemas,
			table_names,
			table_scans,
			true);
			
		auto query_graph_and_max_kernel_id = tree->build_batch_graph(logicalPlan);
		auto query_graph = std::get<0>(query_graph_and_max_kernel_id);
		auto max_kernel_id = std::get<1>(query_graph_and_max_kernel_id);
		auto output = std::shared_ptr<ral::cache::kernel>(new ral::batch::OutputKernel(max_kernel_id, queryContext.clone()));

		if(batchLogger){
            batchLogger->info("{query_id}|{step}|{substep}|{info}|||||",
                              "query_id"_a=queryContext.getContextToken(),
                              "step"_a=queryContext.getQueryStep(),
                              "substep"_a=queryContext.getQuerySubstep(),
                              "info"_a="\"Query Start\n{}\""_format(tree->to_string()));
		}

        auto& communicationData = ral::communication::CommunicationData::getInstance();
        CodeTimer eventTimer(true);
        if(queriesLogger){
            queriesLogger->info("{ral_id}|{query_id}|{start_time}|{plan}|{sql}",
                         "ral_id"_a=queryContext.getNodeIndex(communicationData.getSelfNode()),
                         "query_id"_a=queryContext.getContextToken(),
                         "start_time"_a=eventTimer.start_time(),
                         "plan"_a=tree->to_string(),
                         "sql"_a="'" + sql + "'");
        }

		std::string tables_info = "";
		for (size_t i = 0; i < table_names.size(); i++){
			int num_files = schemas[i].get_files().size();
			int num_rowgroups = schemas[i].get_total_num_rowgroups();
			if (num_files > 0){
				if (num_rowgroups > 0){
					tables_info += "Table " + table_names[i] + ": num files = " + std::to_string(num_files) + "; " + ": num rowgroups = " + std::to_string(num_rowgroups);
				} else {
					tables_info += "Table " + table_names[i] + ": num files = " + std::to_string(num_files) + "; ";
				}				
			} else {
				int num_partitions = input_loaders[i].get_provider()->get_num_handles();
				if (num_partitions > 0){
					tables_info += "Table " + table_names[i] + ": num partitions = " + std::to_string(num_partitions) + "; ";
				} else {
					tables_info += "Table " + table_names[i] + ": empty table; ";
				}
			}
		}
		if(batchLogger){
            batchLogger->info("{query_id}|{step}|{substep}|{info}|||||",
                              "query_id"_a=queryContext.getContextToken(),
                              "step"_a=queryContext.getQueryStep(),
                              "substep"_a=queryContext.getQuerySubstep(),
                              "info"_a="\"" + tables_info + "\"");
		}

		std::map<std::string, std::string> config_options = queryContext.getConfigOptions();
		// Lets build a string with all the configuration parameters set.
		std::string config_info = "";
		std::map<std::string, std::string>::iterator it = config_options.begin();
		while (it != config_options.end())
		{
			config_info += it->first + ": " + it->second + "; ";
			it++;
		}
		if(batchLogger){
            batchLogger->info("{query_id}|{step}|{substep}|{info}|{duration}||||",
                              "query_id"_a=queryContext.getContextToken(),
                              "step"_a=queryContext.getQueryStep(),
                              "substep"_a=queryContext.getQuerySubstep(),
                              "info"_a="\"Config Options: {}\""_format(config_info),
                              "duration"_a="");
		}

		if (query_graph->num_nodes() > 0) {
			ral::cache::cache_settings cache_machine_config;
			cache_machine_config.type = queryContext.getTotalNodes() == 1 ? ral::cache::CacheType::CONCATENATING : ral::cache::CacheType::SIMPLE;
			cache_machine_config.context = queryContext.clone();
			cache_machine_config.concat_all = true;

			query_graph->addPair(ral::cache::kpair(query_graph->get_last_kernel(), output, cache_machine_config));
			// query_graph.show();

			// useful when the Algebra Relacional only contains: ScanTable (or BindableScan) and Limit
			query_graph->check_for_simple_scan_with_limit_query();
		}
		query_graph->check_and_complete_work_flow();
		query_graph->set_kernels_order();

		auto  mem_monitor = std::make_shared<ral::MemoryMonitor>(tree,config_options);
		query_graph->set_memory_monitor(mem_monitor);
		return query_graph;
	} catch(const std::exception& e) {
		if(batchLogger){
            batchLogger->error("{query_id}|{step}|{substep}|{info}|{duration}||||",
                               "query_id"_a=queryContext.getContextToken(),
                               "step"_a=queryContext.getQueryStep(),
                               "substep"_a=queryContext.getQuerySubstep(),
                               "info"_a="In generate_graph. What: {}"_format(e.what()),
                               "duration"_a="");
		}
		throw;
	}
}

void start_execute_graph(std::shared_ptr<ral::cache::graph> graph) {
	CodeTimer blazing_timer;
    std::shared_ptr<spdlog::logger> logger = spdlog::get("batch_logger");
	uint32_t context_token = graph->get_last_kernel()->get_context()->getContextToken();

	try {

		size_t max_kernel_run_threads = 16; //default
		std::map<std::string, std::string> config_options = graph->get_last_kernel()->get_context()->getConfigOptions();
		auto it = config_options.find("MAX_KERNEL_RUN_THREADS");
		if (it != config_options.end()){
			max_kernel_run_threads = std::stoi(config_options["MAX_KERNEL_RUN_THREADS"]);
		}

		graph->start_execute(max_kernel_run_threads);

	} catch(const std::exception& e) {
	    if(logger){
            logger->error("{query_id}|{step}|{substep}|{info}|{duration}||||",
                                        "query_id"_a=context_token,
                                        "step"_a="",
                                        "substep"_a="",
                                        "info"_a="In start_execute_graph. What: {}"_format(e.what()),
                                        "duration"_a="");
	    }
		throw;
	}
}

std::vector<std::unique_ptr<ral::frame::BlazingTable>> get_execute_graph_results(std::shared_ptr<ral::cache::graph> graph) {
	CodeTimer blazing_timer;
    std::shared_ptr<spdlog::logger> logger = spdlog::get("batch_logger");
	uint32_t context_token = graph->get_last_kernel()->get_context()->getContextToken();

	try {

		graph->finish_execute();

		auto output_frame = static_cast<ral::batch::OutputKernel&>(*(graph->get_last_kernel())).release();
		assert(!output_frame.empty());

		if(logger){
            logger->info("{query_id}|{step}|{substep}|{info}|{duration}||||",
                                        "query_id"_a=context_token,
                                        "step"_a="",
                                        "substep"_a="",
                                        "info"_a="Query Execution Done",
                                        "duration"_a=blazing_timer.elapsed_time());
            logger->flush();
		}

		return output_frame;
	} catch(const std::exception& e) {
	    if(logger){
            logger->error("{query_id}|{step}|{substep}|{info}|{duration}||||",
                                        "query_id"_a=context_token,
                                        "step"_a="",
                                        "substep"_a="",
                                        "info"_a="In get_execute_graph_results. What: {}"_format(e.what()),
                                        "duration"_a="");
	    }
		throw;
	}
}

void getTableScanInfo(std::string & logicalPlan_in,
						std::vector<std::string> & relational_algebra_steps_out,
						std::vector<std::string> & table_names_out,
						std::vector<std::vector<int>> & table_columns_out){

	std::vector<std::string> splitted = StringUtil::split(logicalPlan_in, "\n");
	if (splitted[splitted.size() - 1].length() == 0) {
		splitted.erase(splitted.end() -1);
	}

	for (auto step : splitted){
		if (is_scan(step)) {
			relational_algebra_steps_out.push_back(step);

			std::string table_name = extract_table_name(step);
			if(StringUtil::beginsWith(table_name, "main.")) {
				table_name = table_name.substr(5);
			}
			table_names_out.push_back(table_name);

			if (is_bindable_scan(step)) {
				std::string projects = get_named_expression(step, "projects");
				std::vector<std::string> column_index_strings = get_expressions_from_expression_list(projects, true);
				std::vector<int> column_indeces;
				std::transform(column_index_strings.begin(), column_index_strings.end(), std::back_inserter(column_indeces), [](const std::string& str) { return std::stoi(str); });
				table_columns_out.push_back(column_indeces);
			}else if (is_scan(step)){
				table_columns_out.push_back({});
			}
		}
	}
}
