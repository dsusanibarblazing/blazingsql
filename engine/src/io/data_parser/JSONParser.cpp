#include <arrow/io/file.h>
#include <blazingdb/io/Library/Logging/Logger.h>
#include <numeric>

#include "ArgsUtil.h"
#include "JSONParser.h"

namespace ral {
namespace io {

json_parser::json_parser(std::map<std::string, std::string> args_map_) : args_map{args_map_} {}

json_parser::~json_parser() {

}

std::unique_ptr<ral::frame::BlazingTable> json_parser::parse_batch(ral::io::data_handle handle,
	const Schema & schema,
	std::vector<int> column_indices,
	std::vector<cudf::size_type> /*row_groups*/) {
	std::shared_ptr<arrow::io::RandomAccessFile> file = handle.file_handle;
	if(file == nullptr) {
		return schema.makeEmptyBlazingTable(column_indices);
	}

	if(column_indices.size() > 0) {
		auto arrow_source = cudf::io::arrow_io_source{file};
		cudf::io::json_reader_options json_opts = getJsonReaderOptions(args_map, arrow_source);

		cudf::io::table_with_metadata json_table = cudf::io::read_json(json_opts);

		auto columns = json_table.tbl->release();
		auto column_names = std::move(json_table.metadata.column_names);

		// We just need the columns in column_indices
		std::vector<std::unique_ptr<cudf::column>> selected_columns;
		selected_columns.reserve(column_indices.size());
		std::vector<std::string> selected_column_names;
		selected_column_names.reserve(column_indices.size());
		for(auto && i : column_indices) {
			selected_columns.push_back(std::move(columns[i]));
			selected_column_names.push_back(std::move(column_names[i]));
		}

		return std::make_unique<ral::frame::BlazingTable>(
			std::make_unique<cudf::table>(std::move(selected_columns)), selected_column_names);
	}

	return nullptr;
}

void json_parser::parse_schema(std::shared_ptr<arrow::io::RandomAccessFile> file, ral::io::Schema & schema) {
	auto arrow_source = cudf::io::arrow_io_source{file};
	cudf::io::json_reader_options args = getJsonReaderOptions(args_map, arrow_source);

	int64_t num_bytes = file->GetSize().ValueOrDie();

	// lets only read up to 48192 bytes. We are assuming that a full row will always be less than that
	if(num_bytes > 48192) {
		num_bytes = 48192;
	}
	args.set_byte_range_offset(0);
	args.set_byte_range_size(num_bytes);

	cudf::io::table_with_metadata table_and_metadata = cudf::io::read_json(args);
	file->Close();

	for(auto i = 0; i < table_and_metadata.tbl->num_columns(); i++) {
		std::string name = table_and_metadata.metadata.column_names[i];
		cudf::type_id type = table_and_metadata.tbl->get_column(i).type().id();
		size_t file_index = i;
		bool is_in_file = true;
		schema.add_column(name, type, file_index, is_in_file);
	}
}

} /* namespace io */
} /* namespace ral */
