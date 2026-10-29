#include "grib2_index_function.hpp"

#include "grib2_index_file.hpp"

#include "grib2_codes.hpp"
#include "grib2_idx.hpp"
#include "grib2_message.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <string>
#include <vector>

// grib2_index(path, idx := true, details := false)
//
// One row per GRIB message. `path` may be a glob. Three ways of getting the rows:
//
//   * the wgrib2-style ".idx" sidecar next to the file (default when it exists): a single small read, giving the
//     variable, level, forecast and qualifier as NOAA writes them;
//   * hopping through the file with the length in each message's indicator section (no sidecar, or idx := false):
//     a small read per message, giving the parameter numbers, grid and packing templates, but no names;
//   * details := true additionally range-reads a short prefix of every message and asks ecCodes for names, level
//     and grid details.
//
// All reads go through DuckDB's file system, so s3:// and http(s):// paths work when httpfs is loaded.

namespace duckdb {

namespace {

struct Grib2IndexBindData : public TableFunctionData {
	std::string pattern;
	bool use_idx = true;
	bool details = false;
};

struct Grib2IndexGlobalState : public GlobalTableFunctionState {
	std::vector<Grib2IndexRow> rows;
	idx_t position = 0;
};

unique_ptr<FunctionData> Grib2IndexBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<Grib2IndexBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("grib2_index: the path cannot be NULL");
	}
	result->pattern = StringValue::Get(input.inputs[0]);
	for (auto &parameter : input.named_parameters) {
		if (parameter.second.IsNull()) {
			throw InvalidInputException("grib2_index: parameter '" + parameter.first.GetIdentifierName() +
			                            "' cannot be NULL");
		}
		// Identifiers compare case-insensitively, so IDX := false works too
		if (parameter.first == "idx") {
			result->use_idx = BooleanValue::Get(parameter.second);
		} else if (parameter.first == "details") {
			result->details = BooleanValue::Get(parameter.second);
		}
	}

	const std::vector<std::pair<const char *, LogicalType>> columns = {
	    {"file_path", LogicalType::VARCHAR},
	    {"message_number", LogicalType::BIGINT},
	    {"byte_offset", LogicalType::BIGINT},
	    {"byte_length", LogicalType::BIGINT},
	    {"reference_time", LogicalType::TIMESTAMP},
	    {"variable", LogicalType::VARCHAR},
	    {"level", LogicalType::VARCHAR},
	    {"forecast", LogicalType::VARCHAR},
	    {"qualifier", LogicalType::VARCHAR},
	    {"inventory", LogicalType::VARCHAR},
	    {"step_hours", LogicalType::BIGINT},
	    {"percentile", LogicalType::DOUBLE},
	    {"threshold_lower", LogicalType::DOUBLE},
	    {"threshold_upper", LogicalType::DOUBLE},
	    {"discipline", LogicalType::INTEGER},
	    {"parameter_category", LogicalType::INTEGER},
	    {"parameter_number", LogicalType::INTEGER},
	    {"grid_template", LogicalType::INTEGER},
	    {"packing_template", LogicalType::INTEGER},
	    {"packing_type", LogicalType::VARCHAR},
	    {"short_name", LogicalType::VARCHAR},
	    {"type_of_level", LogicalType::VARCHAR},
	    {"level_value", LogicalType::DOUBLE},
	    {"grid_type", LogicalType::VARCHAR},
	    {"ni", LogicalType::BIGINT},
	    {"nj", LogicalType::BIGINT},
	    {"alternative_row_scanning", LogicalType::BOOLEAN},
	};
	for (const auto &column : columns) {
		names.emplace_back(column.first);
		return_types.emplace_back(column.second);
	}
	return result;
}

unique_ptr<GlobalTableFunctionState> Grib2IndexInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<Grib2IndexBindData>();
	auto state = make_uniq<Grib2IndexGlobalState>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto files = fs.GlobFiles(bind_data.pattern, FileGlobOptions::DISALLOW_EMPTY);
	std::sort(files.begin(), files.end(), [](const OpenFileInfo &a, const OpenFileInfo &b) { return a.path < b.path; });
	for (const auto &file : files) {
		IndexFile(fs, file.path, bind_data.use_idx, bind_data.details, state->rows);
	}
	return state;
}

//! Write an optional value, as NULL when it is absent
template <class T>
void WriteOptional(VectorWriter<T> &writer, const optional<T> &value) {
	if (value) {
		writer.WriteValue(*value);
	} else {
		writer.WriteNull();
	}
}

void WriteString(VectorWriter<string_t> &writer, const std::string &value) {
	writer.WriteValue(string_t(value.c_str(), UnsafeNumericCast<uint32_t>(value.size())));
}

void WriteOptionalString(VectorWriter<string_t> &writer, const optional<std::string> &value) {
	if (value) {
		WriteString(writer, *value);
	} else {
		writer.WriteNull();
	}
}

void Grib2IndexScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &state = data.global_state->Cast<Grib2IndexGlobalState>();
	const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.rows.size() - state.position);
	{
		// One writer per column, in the order declared in the bind function
		idx_t c = 0;
		auto file_path = FlatVector::Writer<string_t>(output.data[c++], count);
		auto message_number = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto byte_offset = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto byte_length = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto reference_time = FlatVector::Writer<timestamp_t>(output.data[c++], count);
		auto variable = FlatVector::Writer<string_t>(output.data[c++], count);
		auto level = FlatVector::Writer<string_t>(output.data[c++], count);
		auto forecast = FlatVector::Writer<string_t>(output.data[c++], count);
		auto qualifier = FlatVector::Writer<string_t>(output.data[c++], count);
		auto inventory = FlatVector::Writer<string_t>(output.data[c++], count);
		auto step_hours = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto percentile = FlatVector::Writer<double>(output.data[c++], count);
		auto threshold_lower = FlatVector::Writer<double>(output.data[c++], count);
		auto threshold_upper = FlatVector::Writer<double>(output.data[c++], count);
		auto discipline = FlatVector::Writer<int32_t>(output.data[c++], count);
		auto parameter_category = FlatVector::Writer<int32_t>(output.data[c++], count);
		auto parameter_number = FlatVector::Writer<int32_t>(output.data[c++], count);
		auto grid_template = FlatVector::Writer<int32_t>(output.data[c++], count);
		auto packing_template = FlatVector::Writer<int32_t>(output.data[c++], count);
		auto packing_type = FlatVector::Writer<string_t>(output.data[c++], count);
		auto short_name = FlatVector::Writer<string_t>(output.data[c++], count);
		auto type_of_level = FlatVector::Writer<string_t>(output.data[c++], count);
		auto level_value = FlatVector::Writer<double>(output.data[c++], count);
		auto grid_type = FlatVector::Writer<string_t>(output.data[c++], count);
		auto ni = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto nj = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto alternative_row_scanning = FlatVector::Writer<bool>(output.data[c++], count);

		for (idx_t i = 0; i < count; i++) {
			const Grib2IndexRow &row = state.rows[state.position + i];
			WriteString(file_path, row.file_path);
			message_number.WriteValue(row.message_number);
			byte_offset.WriteValue(row.byte_offset);
			byte_length.WriteValue(row.byte_length);
			WriteOptional(reference_time, row.reference_time);
			WriteOptionalString(variable, row.variable);
			WriteOptionalString(level, row.level);
			WriteOptionalString(forecast, row.forecast);
			WriteOptionalString(qualifier, row.qualifier);
			WriteOptionalString(inventory, row.inventory);
			WriteOptional(step_hours, row.step_hours);
			WriteOptional(percentile, row.percentile);
			WriteOptional(threshold_lower, row.threshold_lower);
			WriteOptional(threshold_upper, row.threshold_upper);
			WriteOptional(discipline, row.discipline);
			WriteOptional(parameter_category, row.parameter_category);
			WriteOptional(parameter_number, row.parameter_number);
			WriteOptional(grid_template, row.grid_template);
			WriteOptional(packing_template, row.packing_template);
			WriteOptionalString(packing_type, row.packing_type);
			WriteOptionalString(short_name, row.short_name);
			WriteOptionalString(type_of_level, row.type_of_level);
			WriteOptional(level_value, row.level_value);
			WriteOptionalString(grid_type, row.grid_type);
			WriteOptional(ni, row.ni);
			WriteOptional(nj, row.nj);
			WriteOptional(alternative_row_scanning, row.alternative_row_scanning);
		}
	}
	state.position += count;
	output.SetChildCardinality(count);
}

} // namespace

void RegisterGrib2IndexFunction(ExtensionLoader &loader) {
	TableFunction function("grib2_index", {LogicalType::VARCHAR}, Grib2IndexScan, Grib2IndexBind, Grib2IndexInit);
	function.GetSignature().AddKeywordOnly("idx", LogicalType::BOOLEAN, Value::BOOLEAN(true));
	function.GetSignature().AddKeywordOnly("details", LogicalType::BOOLEAN, Value::BOOLEAN(false));
	loader.RegisterFunction(function);
}

} // namespace duckdb
