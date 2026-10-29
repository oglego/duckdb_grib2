#include "grib2_read_function.hpp"

#include "grib2_codes.hpp"
#include "grib2_grid.hpp"
#include "grib2_index_file.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/function/table_function.hpp"

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

// read_grib2(path, message := ..., search := ..., points := ..., bbox := ..., all_cells := false)
//
// Decoded grid values in long format: one row per selected cell per message.
//
//   Which messages:  all of them, or message := 133 / [133, 134], and/or search := 'TMP:2 m above ground' (a regular
//                    expression, ECMAScript syntax, matched against the .idx inventory text; needs the sidecar).
//   Which cells:     exactly one of
//                      points := [{'lat': 37.21, 'lon': -93.29}, ...]  nearest grid cell to each point
//                      bbox := [lon_min, lat_min, lon_max, lat_max]    every cell centre inside the box
//                      all_cells := true                               the whole grid (3.7 million rows per CONUS
//                      message)
//
// Cells masked out by a bitmap are returned with a NULL value. Points outside the grid produce no row.

namespace duckdb {

namespace {

enum class SpatialMode { POINTS, BBOX, ALL_CELLS };

struct QueryPoint {
	double lat;
	double lon;
};

struct Grib2ReadBindData : public TableFunctionData {
	std::string pattern;
	std::vector<int64_t> messages;
	bool has_search = false;
	std::string search;
	SpatialMode mode = SpatialMode::ALL_CELLS;
	std::vector<QueryPoint> points;
	double bbox[4] = {0, 0, 0, 0}; // lon_min, lat_min, lon_max, lat_max
};

double ToDouble(const Value &value, const std::string &what) {
	if (value.IsNull() || !value.type().IsNumeric()) {
		throw InvalidInputException("read_grib2: " + what + " must be a number");
	}
	return value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
}

std::vector<int64_t> ParseMessages(const Value &value) {
	std::vector<int64_t> messages;
	if (value.IsNull()) {
		throw InvalidInputException("read_grib2: message cannot be NULL");
	}
	std::vector<Value> items;
	if (value.type().id() == LogicalTypeId::LIST) {
		items = ListValue::GetChildren(value);
	} else {
		items.push_back(value);
	}
	for (const Value &item : items) {
		if (item.IsNull() || !item.type().IsIntegral()) {
			throw InvalidInputException("read_grib2: message must be a message number or a list of message numbers");
		}
		const int64_t number = item.DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
		if (number < 1) {
			throw InvalidInputException("read_grib2: message numbers start at 1");
		}
		messages.push_back(number);
	}
	return messages;
}

std::vector<QueryPoint> ParsePoints(const Value &value) {
	const std::string usage = "read_grib2: points must be a list like [{'lat': 37.21, 'lon': -93.29}]";
	if (value.IsNull() || value.type().id() != LogicalTypeId::LIST) {
		throw InvalidInputException(usage);
	}
	std::vector<QueryPoint> points;
	for (const Value &item : ListValue::GetChildren(value)) {
		if (item.IsNull() || item.type().id() != LogicalTypeId::STRUCT) {
			throw InvalidInputException(usage);
		}
		const auto &fields = StructValue::GetChildren(item);
		optional<double> lat;
		optional<double> lon;
		for (idx_t f = 0; f < fields.size(); f++) {
			const Identifier &name = StructType::GetChildName(item.type(), f);
			if (name == "lat") {
				lat = ToDouble(fields[f], "the lat of a point");
			} else if (name == "lon") {
				lon = ToDouble(fields[f], "the lon of a point");
			}
		}
		if (!lat || !lon) {
			throw InvalidInputException(usage);
		}
		if (*lat < -90.0 || *lat > 90.0 || *lon < -360.0 || *lon > 360.0) {
			throw InvalidInputException(
			    "read_grib2: a point is outside the valid range (lat -90 to 90, lon -360 to 360)");
		}
		points.push_back({*lat, *lon});
	}
	if (points.empty()) {
		throw InvalidInputException("read_grib2: points is empty");
	}
	return points;
}

unique_ptr<FunctionData> Grib2ReadBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<Grib2ReadBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("read_grib2: the path cannot be NULL");
	}
	result->pattern = StringValue::Get(input.inputs[0]);

	int modes = 0;
	for (auto &parameter : input.named_parameters) {
		const Value &value = parameter.second;
		if (parameter.first == "message") {
			result->messages = ParseMessages(value);
		} else if (parameter.first == "search") {
			if (value.IsNull()) {
				throw InvalidInputException("read_grib2: search cannot be NULL");
			}
			result->has_search = true;
			result->search = StringValue::Get(value);
			try {
				std::regex check(result->search, std::regex::ECMAScript);
			} catch (const std::regex_error &error) {
				throw InvalidInputException("read_grib2: search is not a valid regular expression: " +
				                            std::string(error.what()));
			}
		} else if (parameter.first == "points") {
			result->mode = SpatialMode::POINTS;
			result->points = ParsePoints(value);
			modes++;
		} else if (parameter.first == "bbox") {
			if (value.IsNull() || value.type().id() != LogicalTypeId::LIST ||
			    ListValue::GetChildren(value).size() != 4) {
				throw InvalidInputException("read_grib2: bbox must be [lon_min, lat_min, lon_max, lat_max]");
			}
			const auto &items = ListValue::GetChildren(value);
			for (idx_t b = 0; b < 4; b++) {
				result->bbox[b] = ToDouble(items[b], "every bbox value");
			}
			if (result->bbox[1] < -90.0 || result->bbox[3] > 90.0 || result->bbox[1] > result->bbox[3]) {
				throw InvalidInputException(
				    "read_grib2: bbox latitudes must satisfy -90 <= lat_min <= lat_max <= 90 (the order is "
				    "[lon_min, lat_min, lon_max, lat_max])");
			}
			result->mode = SpatialMode::BBOX;
			modes++;
		} else if (parameter.first == "all_cells") {
			if (value.IsNull()) {
				throw InvalidInputException("read_grib2: all_cells cannot be NULL");
			}
			if (BooleanValue::Get(value)) {
				result->mode = SpatialMode::ALL_CELLS;
				modes++;
			}
		}
	}
	if (modes == 0) {
		throw InvalidInputException("read_grib2 needs one of points, bbox or all_cells := true: a single NBM CONUS "
		                            "message has 3.7 million cells");
	}
	if (modes > 1) {
		throw InvalidInputException("read_grib2: choose only one of points, bbox and all_cells");
	}

	const std::vector<std::pair<const char *, LogicalType>> columns = {
	    {"file_path", LogicalType::VARCHAR},
	    {"message_number", LogicalType::BIGINT},
	    {"reference_time", LogicalType::TIMESTAMP},
	    {"valid_time", LogicalType::TIMESTAMP},
	    {"variable", LogicalType::VARCHAR},
	    {"level", LogicalType::VARCHAR},
	    {"forecast", LogicalType::VARCHAR},
	    {"qualifier", LogicalType::VARCHAR},
	    {"short_name", LogicalType::VARCHAR},
	    {"lat", LogicalType::DOUBLE},
	    {"lon", LogicalType::DOUBLE},
	    {"i", LogicalType::BIGINT},
	    {"j", LogicalType::BIGINT},
	    {"value", LogicalType::DOUBLE},
	    {"query_lat", LogicalType::DOUBLE},
	    {"query_lon", LogicalType::DOUBLE},
	    {"distance_km", LogicalType::DOUBLE},
	};
	for (const auto &column : columns) {
		names.emplace_back(column.first);
		return_types.emplace_back(column.second);
	}
	return result;
}

struct PointHit {
	idx_t point_index;
	GridCell cell;
};

struct ResultRow {
	idx_t message_index;
	double lat;
	double lon;
	int64_t i;
	int64_t j;
	optional<double> value;
	optional<double> query_lat;
	optional<double> query_lon;
	optional<double> distance_km;
};

struct Grib2ReadGlobalState : public GlobalTableFunctionState {
	//! The selected messages, in file order
	std::vector<Grib2IndexRow> messages;
	//! ecCodes' short name per selected message, filled in as each is loaded
	std::vector<optional<std::string>> short_names;

	idx_t message_position = 0;
	unique_ptr<FileReader> reader;

	// The message currently being emitted
	bool loaded = false;
	idx_t cell_position = 0;
	idx_t cell_count = 0;
	std::unique_ptr<ProjectedGrid> grid;
	bool alternative_rows = false;
	bool has_bitmap = false;
	double missing_value = 9999.0;
	std::vector<double> values;

	// Cell selection, reused while consecutive messages share a grid
	std::string selection_fingerprint;
	std::vector<PointHit> point_hits;
	std::vector<int64_t> box_cells;
};

unique_ptr<GlobalTableFunctionState> Grib2ReadInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<Grib2ReadBindData>();
	auto state = make_uniq<Grib2ReadGlobalState>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto files = fs.GlobFiles(bind_data.pattern, FileGlobOptions::DISALLOW_EMPTY);
	std::sort(files.begin(), files.end(), [](const OpenFileInfo &a, const OpenFileInfo &b) { return a.path < b.path; });

	std::regex search;
	if (bind_data.has_search) {
		search = std::regex(bind_data.search, std::regex::ECMAScript);
	}
	for (const auto &file : files) {
		std::vector<Grib2IndexRow> rows;
		IndexFile(fs, file.path, true, false, rows);
		for (auto &row : rows) {
			if (!bind_data.messages.empty() && std::find(bind_data.messages.begin(), bind_data.messages.end(),
			                                             row.message_number) == bind_data.messages.end()) {
				continue;
			}
			if (bind_data.has_search) {
				if (!row.inventory) {
					throw InvalidInputException("read_grib2: search needs the .idx file that goes with " + file.path +
					                            " (it holds the variable and level names); select messages with "
					                            "message := ... instead");
				}
				if (!std::regex_search(*row.inventory, search)) {
					continue;
				}
			}
			state->messages.push_back(std::move(row));
		}
	}
	state->short_names.resize(state->messages.size());
	return state;
}

//! Read and decode the next message and work out which cells to emit
void LoadMessage(ClientContext &context, const Grib2ReadBindData &bind_data, Grib2ReadGlobalState &state) {
	const char *function = "read_grib2";
	const Grib2IndexRow &row = state.messages[state.message_position];
	if (!state.reader || state.reader->path != row.file_path) {
		state.reader = make_uniq<FileReader>(FileSystem::GetFileSystem(context), row.file_path);
	}
	try {
		const std::string bytes = state.reader->ReadAt(row.byte_offset, row.byte_length);
		CodesHandlePtr handle = OpenGribMessage(function, bytes.data(), bytes.size());
		codes_handle *h = handle.get();

		Grib2Grid grib_grid = MakeGrid(h, function);
		state.values = DecodeGribValues(h, function);
		const int64_t cell_total = grib_grid.grid->Ni() * grib_grid.grid->Nj();
		if (static_cast<int64_t>(state.values.size()) != cell_total) {
			throw InvalidInputException(std::string(function) + ": the message has " +
			                            std::to_string(state.values.size()) + " values but its grid has " +
			                            std::to_string(cell_total) + " cells");
		}
		state.alternative_rows = grib_grid.alternative_rows;
		state.has_bitmap = Grib2LongKeyOrDefault(h, "bitmapPresent", 0) != 0;
		state.missing_value = Grib2OptionalDouble(h, "missingValue").value_or(9999.0);
		state.short_names[state.message_position] = Grib2OptionalString(h, "shortName");

		// Cell selection depends only on the grid, so keep it while messages share one
		const bool same_grid = state.grid && state.selection_fingerprint == grib_grid.fingerprint;
		state.grid = std::move(grib_grid.grid);
		if (!same_grid) {
			state.selection_fingerprint = grib_grid.fingerprint;
			state.point_hits.clear();
			state.box_cells.clear();
			if (bind_data.mode == SpatialMode::POINTS) {
				for (idx_t p = 0; p < bind_data.points.size(); p++) {
					GridCell cell;
					if (state.grid->NearestCell(bind_data.points[p].lat, bind_data.points[p].lon, cell)) {
						state.point_hits.push_back({p, cell});
					}
				}
			} else if (bind_data.mode == SpatialMode::BBOX) {
				state.grid->CellsInBox(bind_data.bbox[0], bind_data.bbox[1], bind_data.bbox[2], bind_data.bbox[3],
				                       state.box_cells);
			}
		}
		switch (bind_data.mode) {
		case SpatialMode::POINTS:
			state.cell_count = state.point_hits.size();
			break;
		case SpatialMode::BBOX:
			state.cell_count = state.box_cells.size();
			break;
		default:
			state.cell_count = static_cast<idx_t>(cell_total);
			break;
		}
		state.cell_position = 0;
		state.loaded = true;
	} catch (std::exception &error) {
		throw InvalidInputException(row.file_path + ": message " + std::to_string(row.message_number) + ": " +
		                            error.what());
	}
}

ResultRow MakeRow(const Grib2ReadBindData &bind_data, const Grib2ReadGlobalState &state, idx_t cell_index) {
	ResultRow out;
	out.message_index = state.message_position;
	int64_t i = 0;
	int64_t j = 0;
	const int64_t ni = state.grid->Ni();
	switch (bind_data.mode) {
	case SpatialMode::POINTS: {
		const PointHit &hit = state.point_hits[cell_index];
		i = hit.cell.i;
		j = hit.cell.j;
		out.lat = hit.cell.lat;
		out.lon = hit.cell.lon;
		out.query_lat = bind_data.points[hit.point_index].lat;
		out.query_lon = bind_data.points[hit.point_index].lon;
		out.distance_km = hit.cell.distance_km;
		break;
	}
	case SpatialMode::BBOX: {
		const int64_t flat = state.box_cells[cell_index];
		i = flat % ni;
		j = flat / ni;
		state.grid->Inverse(static_cast<double>(i), static_cast<double>(j), out.lat, out.lon);
		break;
	}
	default: {
		const int64_t flat = static_cast<int64_t>(cell_index);
		i = flat % ni;
		j = flat / ni;
		state.grid->Inverse(static_cast<double>(i), static_cast<double>(j), out.lat, out.lon);
		break;
	}
	}
	out.i = i;
	out.j = j;
	const double value = state.values[static_cast<size_t>(RawValueIndex(ni, i, j, state.alternative_rows))];
	if (!(state.has_bitmap && value == state.missing_value)) {
		out.value = value;
	}
	return out;
}

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

void Grib2ReadScan(ClientContext &context, TableFunctionInput &data, DataChunk &output) {
	auto &bind_data = data.bind_data->Cast<Grib2ReadBindData>();
	auto &state = data.global_state->Cast<Grib2ReadGlobalState>();

	// Gather up to a vector of rows, moving on to the next message when the current one is exhausted
	std::vector<ResultRow> batch;
	batch.reserve(STANDARD_VECTOR_SIZE);
	while (batch.size() < STANDARD_VECTOR_SIZE) {
		if (!state.loaded) {
			if (state.message_position >= state.messages.size()) {
				break;
			}
			LoadMessage(context, bind_data, state);
		}
		while (state.cell_position < state.cell_count && batch.size() < STANDARD_VECTOR_SIZE) {
			batch.push_back(MakeRow(bind_data, state, state.cell_position++));
		}
		if (state.cell_position >= state.cell_count) {
			// Rows already gathered refer to this message by index, so only the large buffers are released
			state.values.clear();
			state.values.shrink_to_fit();
			state.loaded = false;
			state.message_position++;
		}
	}

	const idx_t count = batch.size();
	{
		idx_t c = 0;
		auto file_path = FlatVector::Writer<string_t>(output.data[c++], count);
		auto message_number = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto reference_time = FlatVector::Writer<timestamp_t>(output.data[c++], count);
		auto valid_time = FlatVector::Writer<timestamp_t>(output.data[c++], count);
		auto variable = FlatVector::Writer<string_t>(output.data[c++], count);
		auto level = FlatVector::Writer<string_t>(output.data[c++], count);
		auto forecast = FlatVector::Writer<string_t>(output.data[c++], count);
		auto qualifier = FlatVector::Writer<string_t>(output.data[c++], count);
		auto short_name = FlatVector::Writer<string_t>(output.data[c++], count);
		auto lat = FlatVector::Writer<double>(output.data[c++], count);
		auto lon = FlatVector::Writer<double>(output.data[c++], count);
		auto i_writer = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto j_writer = FlatVector::Writer<int64_t>(output.data[c++], count);
		auto value = FlatVector::Writer<double>(output.data[c++], count);
		auto query_lat = FlatVector::Writer<double>(output.data[c++], count);
		auto query_lon = FlatVector::Writer<double>(output.data[c++], count);
		auto distance_km = FlatVector::Writer<double>(output.data[c++], count);

		for (const ResultRow &row : batch) {
			const Grib2IndexRow &message = state.messages[row.message_index];
			WriteString(file_path, message.file_path);
			message_number.WriteValue(message.message_number);
			WriteOptional(reference_time, message.reference_time);
			if (message.reference_time && message.step_hours) {
				valid_time.WriteValue(timestamp_t(message.reference_time->value + *message.step_hours * 3600000000LL));
			} else {
				valid_time.WriteNull();
			}
			WriteOptionalString(variable, message.variable);
			WriteOptionalString(level, message.level);
			WriteOptionalString(forecast, message.forecast);
			WriteOptionalString(qualifier, message.qualifier);
			WriteOptionalString(short_name, state.short_names[row.message_index]);
			lat.WriteValue(row.lat);
			lon.WriteValue(row.lon);
			i_writer.WriteValue(row.i);
			j_writer.WriteValue(row.j);
			WriteOptional(value, row.value);
			WriteOptional(query_lat, row.query_lat);
			WriteOptional(query_lon, row.query_lon);
			WriteOptional(distance_km, row.distance_km);
		}
	}
	output.SetChildCardinality(count);
}

} // namespace

void RegisterGrib2ReadFunction(ExtensionLoader &loader) {
	TableFunction function("read_grib2", {LogicalType::VARCHAR}, Grib2ReadScan, Grib2ReadBind, Grib2ReadInit);
	auto &signature = function.GetSignature();
	signature.AddKeywordOnly("message", LogicalType::ANY);
	signature.AddKeywordOnly("search", LogicalType::VARCHAR);
	signature.AddKeywordOnly("points", LogicalType::ANY);
	signature.AddKeywordOnly("bbox", LogicalType::ANY);
	signature.AddKeywordOnly("all_cells", LogicalType::BOOLEAN, Value::BOOLEAN(false));
	loader.RegisterFunction(function);
}

} // namespace duckdb
