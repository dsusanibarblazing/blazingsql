#pragma once

#include "kernel.h"
#include "kpair.h"
#include "execution_graph/logic_controllers/CacheMachine.h"
#include "bmr/MemoryMonitor.h"
#include "utilities/ctpl_stl.h"
namespace ral {
namespace cache {

class kernel;

static std::shared_ptr<ral::cache::CacheMachine> create_cache_machine( const cache_settings& config, std::string cache_machine_name) {
	std::shared_ptr<ral::cache::CacheMachine> machine;
	if (config.type == CacheType::SIMPLE or config.type == CacheType::FOR_EACH) {
		machine =  std::make_shared<ral::cache::CacheMachine>(config.context, cache_machine_name);
	} else if (config.type == CacheType::CONCATENATING) {
		machine =  std::make_shared<ral::cache::ConcatenatingCacheMachine>(config.context, 
			config.concat_cache_num_bytes, config.concat_all, cache_machine_name);
	}
	return machine;
}

[[maybe_unused]] static std::vector<std::shared_ptr<ral::cache::CacheMachine>> create_cache_machines(const cache_settings& config, std::string source_port, int32_t source_kernel_id) {
	std::vector<std::shared_ptr<ral::cache::CacheMachine>> machines;
	if (config.num_partitions > 1){
		for (size_t i = 0; i < config.num_partitions; i++) {
			std::string cache_machine_name = std::to_string(source_kernel_id) + "_" + source_port + "_" + std::to_string(i);
			machines.push_back(create_cache_machine(config, cache_machine_name));
		}
	} else {
		std::string cache_machine_name = std::to_string(source_kernel_id) + "_" + source_port;
		machines.push_back(create_cache_machine(config, cache_machine_name));
	}
	return machines;
}

struct graph_progress {
	std::vector<std::string> kernel_descriptions;
    std::vector<bool> finished;
    std::vector<int> batches_completed;
};

/**
	@brief A class that represents the execution graph in a taskflow scheme.
	The taskflow scheme is basically implemeted by the execution graph and the kernels associated to each node in the graph.
*/
class graph {
protected:
	struct Edge {
		std::int32_t source;
		std::int32_t target;
		std::string source_port_name;
		std::string target_port_name;

		bool operator<(const Edge & e) const { return this->target < e.target || (this->target == e.target && this->source < e.source); }
		bool operator==(const Edge & e) const { return this->target == e.target && this->source == e.source; }

		void print() const {
			std::cout<<"Edge: source id: "<<source<<" name: "<<source_port_name<<" target id: "<<target<<" name: "<<target_port_name<<std::endl;
		}
	};

public:
	graph() {
		container_[head_id_] = nullptr;	 // sentinel node
		kernels_edges_logger = spdlog::get("kernels_edges_logger");
	}
	~graph() {}
	graph(const graph &) = default;
	graph & operator=(const graph &) = default;

	int32_t get_context_token();

	//TODO: add to constructor when i am not exhausted and want to fix all the tests
	//this would impact
	void set_context_token(int32_t token);

	void addPair(kpair p);

	void check_and_complete_work_flow();

	void start_execute(const std::size_t max_kernel_run_threads);
	void finish_execute();

	void show();

	void show_from_kernel (int32_t id);

	std::pair<bool, uint64_t> get_estimated_input_rows_to_kernel(int32_t id);

	std::pair<bool, uint64_t> get_estimated_input_rows_to_cache(int32_t id, const std::string & port_name);

	std::shared_ptr<kernel> get_last_kernel();

	bool query_is_complete();

	graph_progress get_progress();

	size_t num_nodes() const;

	size_t add_node(std::shared_ptr<kernel> k);

	void add_edge(std::shared_ptr<kernel> source,
		std::shared_ptr<kernel> target,
		std::string source_port,
		std::string target_port,
		const cache_settings & config);

	kernel * get_node(size_t id);
	std::shared_ptr<ral::cache::CacheMachine>  get_kernel_output_cache(size_t kernel_id, std::string cache_id = "");

	void set_input_and_output_caches(std::shared_ptr<ral::cache::CacheMachine> input_cache, std::shared_ptr<ral::cache::CacheMachine> output_cache);
	std::shared_ptr<ral::cache::CacheMachine> get_input_message_cache();
	std::shared_ptr<ral::cache::CacheMachine> get_output_message_cache();

	std::set<Edge> get_neighbours(kernel * from);
	std::set<Edge> get_neighbours(int32_t id);
	std::set<Edge> get_reverse_neighbours(kernel * from);
	std::set<Edge> get_reverse_neighbours(int32_t id);

	void set_kernels_order();

	void check_for_simple_scan_with_limit_query();
	void set_memory_monitor(std::shared_ptr<ral::MemoryMonitor> mem_monitor);
	void clear_kernels(); 
	
private:
	const std::int32_t head_id_{-1};
	std::vector<kernel *> kernels_;
	std::map<std::int32_t, std::shared_ptr<kernel>> container_;
	std::map<std::int32_t, std::set<Edge>> edges_;
	std::map<std::int32_t, std::set<Edge>> reverse_edges_;

	std::shared_ptr<ral::cache::CacheMachine> input_cache_;
	std::shared_ptr<ral::cache::CacheMachine> output_cache_;

	std::shared_ptr<spdlog::logger> kernels_edges_logger;
	int32_t context_token;
	std::shared_ptr<ral::MemoryMonitor> mem_monitor;
	ctpl::thread_pool<BlazingThread> pool;
	std::vector<std::future<void>> futures;
	std::vector<int32_t> ordered_kernel_ids;  // ordered vector containing the kernel_ids in the order they will be started
};

}  // namespace cache
}  // namespace ral
