#include "grib2_index_function.hpp"

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

struct Grib2IndexRow {
	std::string file_path;
	int64_t message_number = 0;
	int64_t byte_offset = 0;
	int64_t byte_length = 0;
	optional<timestamp_t> reference_time;

	// From the .idx sidecar
	optional<std::string> variable;
	optional<std::string> level;
	optional<std::string> forecast;
	optional<std::string> qualifier;
	optional<std::string> inventory;
	optional<int64_t> step_hours;
	optional<double> percentile;
	optional<double> threshold_lower;
	optional<double> threshold_upper;

	// From the message's section headers
	optional<int32_t> discipline;
	optional<int32_t> parameter_category;
	optional<int32_t> parameter_number;
	optional<int32_t> grid_template;
	optional<int32_t> packing_template;
	optional<std::string> packing_type;

	// From ecCodes, details := true only
	optional<std::string> short_name;
	optional<std::string> type_of_level;
	optional<double> level_value;
	optional<std::string> grid_type;
	optional<int64_t> ni;
	optional<int64_t> nj;
	optional<bool> alternative_row_scanning;
};

struct Grib2IndexBindData : public TableFunctionData {
	std::string pattern;
	bool use_idx = true;
	bool details = false;
};

struct Grib2IndexGlobalState : public GlobalTableFunctionState {
	std::vector<Grib2IndexRow> rows;
	idx_t position = 0;
};

//! A file opened for random access reads
class FileReader {
public:
	FileReader(FileSystem &fs, const std::string &path_p)
	    : path(path_p), handle(fs.OpenFile(path_p, FileFlags::FILE_FLAGS_READ)),
	      size(static_cast<int64_t>(handle->GetFileSize())) {
	}

	std::string ReadAt(int64_t offset, int64_t length) const {
		if (offset < 0 || length < 0 || offset + length > size) {
			throw IOException(path + ": cannot read " + std::to_string(length) + " bytes at offset " +
			                  std::to_string(offset) + " (the file is " + std::to_string(size) + " bytes)");
		}
		std::string buffer(static_cast<size_t>(length), '\0');
		if (length > 0) {
			handle->Read(&buffer[0], static_cast<idx_t>(length), static_cast<idx_t>(offset));
		}
		return buffer;
	}

	std::string path;
	unique_ptr<FileHandle> handle;
	int64_t size;
};

optional<timestamp_t> MakeTimestamp(int year, int month, int day, int hour, int minute, int second) {
	date_t date;
	if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 ||
	    !Date::TryFromDate(year, month, day, date)) {
		return optional<timestamp_t>();
	}
	return Timestamp::FromDatetime(date, Time::FromTime(hour, minute, second));
}

//! Read the section headers of the message at `offset`. Starts with a short prefix and grows it as the parser asks.
//! known_length is the message length when it is already known, or -1. On success `prefix` holds the bytes read.
bool ReadSections(const FileReader &file, int64_t offset, int64_t known_length, MessageSummary &summary,
                  std::string &prefix, std::string &error) {
	const int64_t remaining = file.size - offset;
	int64_t limit = known_length >= 0 ? std::min(known_length, remaining) : remaining;
	int64_t want = std::min<int64_t>(512, limit);
	for (int round = 0; round < 32; round++) {
		prefix = file.ReadAt(offset, want);
		summary = MessageSummary();
		size_t needed = 0;
		const SectionParseResult result = ParseMessageSections(reinterpret_cast<const uint8_t *>(prefix.data()),
		                                                       prefix.size(), summary, needed, error);
		if (result == SectionParseResult::OK) {
			return true;
		}
		if (result == SectionParseResult::INVALID) {
			return false;
		}
		if (summary.total_length > 0 && static_cast<int64_t>(summary.total_length) < limit) {
			limit = static_cast<int64_t>(summary.total_length);
		}
		if (static_cast<int64_t>(needed) <= want || want >= limit) {
			error = "the file ends inside this message (truncated?)";
			return false;
		}
		want = std::min<int64_t>(static_cast<int64_t>(needed), limit);
	}
	error = "could not read the message headers";
	return false;
}

void FillFromSections(Grib2IndexRow &row, const MessageSummary &summary, bool set_reference_time) {
	row.discipline = summary.discipline;
	row.parameter_category = summary.parameter_category;
	row.parameter_number = summary.parameter_number;
	row.grid_template = summary.grid_template;
	row.packing_template = summary.data_template;
	const char *name = DataTemplateName(summary.data_template);
	row.packing_type = name ? std::string(name) : "template " + std::to_string(summary.data_template);
	if (set_reference_time) {
		row.reference_time =
		    MakeTimestamp(summary.year, summary.month, summary.day, summary.hour, summary.minute, summary.second);
	}
}

//! Names, level and grid details from ecCodes, using only the short prefix already read
void FillDetails(Grib2IndexRow &row, const std::string &prefix) {
	CodesHandlePtr handle(codes_handle_new_from_partial_message_copy(nullptr, prefix.data(), prefix.size()));
	if (!handle) {
		return;
	}
	codes_handle *h = handle.get();
	row.short_name = Grib2OptionalString(h, "shortName");
	row.type_of_level = Grib2OptionalString(h, "typeOfLevel");
	row.level_value = Grib2OptionalDouble(h, "level");
	row.grid_type = Grib2OptionalString(h, "gridType");
	row.ni = Grib2OptionalLong(h, "Ni");
	row.nj = Grib2OptionalLong(h, "Nj");
	const optional<int64_t> alternative_rows = Grib2OptionalLong(h, "alternativeRowScanning");
	if (alternative_rows) {
		row.alternative_row_scanning = *alternative_rows != 0;
	}
}

[[noreturn]] void ThrowBadMessage(const std::string &path, int64_t message_number, int64_t offset,
                                  const std::string &error) {
	throw InvalidInputException(path + ": message " + std::to_string(message_number) + " at byte offset " +
	                            std::to_string(offset) + ": " + error);
}

void IndexFile(FileSystem &fs, const std::string &path, bool use_idx, bool details, std::vector<Grib2IndexRow> &rows) {
	const FileReader file(fs, path);
	const std::string idx_path = path + ".idx";

	if (use_idx && fs.FileExists(idx_path)) {
		const FileReader idx_file(fs, idx_path);
		std::vector<IdxEntry> entries;
		std::string error;
		if (!ParseIdx(idx_file.ReadAt(0, idx_file.size), file.size, entries, error)) {
			throw InvalidInputException(idx_path + ": " + error);
		}
		for (size_t i = 0; i < entries.size(); i++) {
			const IdxEntry &entry = entries[i];
			Grib2IndexRow row;
			row.file_path = path;
			row.message_number = entry.message_number;
			row.byte_offset = entry.byte_offset;
			row.byte_length = entry.byte_length;
			row.reference_time = MakeTimestamp(entry.year, entry.month, entry.day, entry.hour, entry.minute, 0);
			row.variable = entry.variable;
			row.level = entry.level;
			row.forecast = entry.forecast;
			row.qualifier = entry.qualifier;
			row.inventory = entry.Inventory();
			row.step_hours = entry.step_hours;
			row.percentile = entry.percentile;
			row.threshold_lower = entry.threshold_lower;
			row.threshold_upper = entry.threshold_upper;

			// A sidecar that does not belong to the file would silently give wrong offsets, so check it against the
			// message itself: every message with details, otherwise just the first one.
			if (details || i == 0) {
				MessageSummary summary;
				std::string prefix;
				if (!ReadSections(file, entry.byte_offset, entry.byte_length, summary, prefix, error)) {
					ThrowBadMessage(path, entry.message_number, entry.byte_offset,
					                error + " (does " + idx_path + " belong to this file?)");
				}
				if (static_cast<int64_t>(summary.total_length) != entry.byte_length) {
					ThrowBadMessage(path, entry.message_number, entry.byte_offset,
					                "the message is " + std::to_string(summary.total_length) + " bytes but " +
					                    idx_path + " implies " + std::to_string(entry.byte_length) +
					                    " (does the .idx belong to this file?)");
				}
				if (details) {
					FillFromSections(row, summary, false);
					FillDetails(row, prefix);
				}
			}
			rows.push_back(std::move(row));
		}
		return;
	}

	// No sidecar: hop from message to message using the lengths in the indicator sections
	int64_t offset = 0;
	int64_t number = 1;
	while (offset < file.size) {
		MessageSummary summary;
		std::string prefix;
		std::string error;
		if (!ReadSections(file, offset, -1, summary, prefix, error)) {
			ThrowBadMessage(path, number, offset, error);
		}
		const int64_t length = static_cast<int64_t>(summary.total_length);
		if (offset + length > file.size) {
			ThrowBadMessage(path, number, offset, "the file ends inside this message (truncated?)");
		}
		Grib2IndexRow row;
		row.file_path = path;
		row.message_number = number;
		row.byte_offset = offset;
		row.byte_length = length;
		FillFromSections(row, summary, true);
		if (details) {
			FillDetails(row, prefix);
		}
		rows.push_back(std::move(row));
		offset += length;
		number++;
	}
}

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
