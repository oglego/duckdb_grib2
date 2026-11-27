//! GRIB2 table-function reader for selected messages and cells.
//!
//! This file implements `read_grib2(path, message := ..., search := ..., points := ..., bbox := ..., all_cells :=
//! false)`, which turns a GRIB2 file or glob into long-format SQL rows. The function selects messages by number or
//! inventory text, chooses a spatial mode (nearest point, bounding box, or whole grid), decodes the matching grid
//! values, and emits one row per selected cell. Cells masked by a bitmap are returned as NULL, and points outside the
//! projected grid produce no row. The implementation keeps file I/O overlapped and deterministic while validating
//! message lengths and index metadata before the expensive decode step.

#include "grib2_read_function.hpp"

#include "grib2_codes.hpp"
#include "grib2_grid.hpp"
#include "grib2_index_file.hpp"
#include "grib2_message.hpp"
#include "grib2_settings.hpp"
#include "grib2_valid_time.hpp"
#include "grib2_vector_writers.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/re2_regex.hpp"
#include "duckdb/function/table_function.hpp"

#include "re2/re2.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <future>
#include <memory>
#include <new>
#include <string>
#include <unordered_set>
#include <vector>

// read_grib2(path, message := ..., search := ..., points := ..., bbox := ..., all_cells := false)
//
// Decoded grid values in long format: one row per selected cell per message.
//
//   Which messages:  all of them, or message := 133 / [133, 134], and/or search := 'TMP:2 m above ground' (a RE2
//                    regular expression matched against the .idx inventory text; needs the sidecar).
//   Which cells:     exactly one of
//                      points := [{'lat': 37.21, 'lon': -93.29}, ...]  nearest grid cell to each point
//                      bbox := [lon_min, lat_min, lon_max, lat_max]    every cell centre inside the box; lon_max <
//                                                                      lon_min needs wrap := true
//                      all_cells := true                               the whole grid (3.7 million rows per CONUS
//                      message)
//
// Cells masked out by a bitmap are returned with a NULL value. Points outside the grid produce no row.

namespace duckdb {

namespace {

//! Longest regular expression accepted by search
constexpr size_t MAX_SEARCH_PATTERN_LENGTH = 1024;

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
	//! The box may run east through the antimeridian (lon_max < lon_min). Opt-in: otherwise a swapped pair of
	//! longitudes would silently select almost the whole globe.
	bool bbox_wrap = false;
};

double ToDouble(const Value &value, const std::string &what) {
	if (value.IsNull() || !value.type().IsNumeric()) {
		throw InvalidInputException("read_grib2: " + what + " must be a number");
	}
	return value.DefaultCastAs(LogicalType::DOUBLE).GetValue<double>();
}

//! Range tests shared by points and bbox. NaN is not out of range here; bbox refuses non-finite values separately.
bool LatitudeOutOfRange(double lat) {
	return lat < -90.0 || lat > 90.0;
}

bool LongitudeOutOfRange(double lon) {
	return lon < -360.0 || lon > 360.0;
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
	if (items.empty()) {
		// An empty list would otherwise mean "no filter" and silently read every message
		throw InvalidInputException("read_grib2: message is an empty list; leave it out to read every message");
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
		if (!std::isfinite(*lat) || !std::isfinite(*lon) || LatitudeOutOfRange(*lat) || LongitudeOutOfRange(*lon)) {
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

//! Compile a search pattern, refusing one that is too long or not valid RE2 syntax. Bind calls it to report a bad
//! pattern early; init calls it again for the compiled expression.
unique_ptr<duckdb_re2::RE2> CompileSearch(const std::string &pattern) {
	// Bound compile cost and memory even though RE2 guarantees linear-time matching.
	if (pattern.size() > MAX_SEARCH_PATTERN_LENGTH) {
		throw InvalidInputException("read_grib2: search is longer than " + std::to_string(MAX_SEARCH_PATTERN_LENGTH) +
		                            " characters");
	}
	duckdb_re2::RE2::Options options;
	options.set_log_errors(false);
	auto regex = make_uniq<duckdb_re2::RE2>(pattern, options);
	if (!regex->ok()) {
		throw InvalidInputException("read_grib2: search is not a valid regular expression: " + regex->error());
	}
	return regex;
}

unique_ptr<FunctionData> Grib2ReadBind(ClientContext &context, TableFunctionBindInput &input,
                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<Grib2ReadBindData>();
	if (input.inputs[0].IsNull()) {
		throw InvalidInputException("read_grib2: the path cannot be NULL");
	}
	result->pattern = StringValue::Get(input.inputs[0]);

	int modes = 0;
	bool wrap_given = false;
	for (auto &parameter : input.named_parameters) {
		const Value &value = parameter.second;
		if (parameter.first == "wrap") {
			if (value.IsNull()) {
				throw InvalidInputException("read_grib2: wrap cannot be NULL");
			}
			result->bbox_wrap = BooleanValue::Get(value);
			wrap_given = result->bbox_wrap;
			continue;
		}
		if (parameter.first == "all_cells") {
			if (value.IsNull()) {
				throw InvalidInputException("read_grib2: all_cells cannot be NULL");
			}
			if (BooleanValue::Get(value)) {
				result->mode = SpatialMode::ALL_CELLS;
				modes++;
			}
			continue;
		}
		if (value.IsNull()) {
			// Optional parameters default to NULL, which means "not given"
			continue;
		}
		if (parameter.first == "message") {
			result->messages = ParseMessages(value);
		} else if (parameter.first == "search") {
			result->has_search = true;
			result->search = StringValue::Get(value);
			CompileSearch(result->search); // reports a bad pattern now rather than when the scan starts
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
			for (const double coordinate : result->bbox) {
				if (!std::isfinite(coordinate)) {
					throw InvalidInputException("read_grib2: bbox coordinates must be finite");
				}
			}
			if (LatitudeOutOfRange(result->bbox[1]) || LatitudeOutOfRange(result->bbox[3]) ||
			    result->bbox[1] > result->bbox[3]) {
				throw InvalidInputException(
				    "read_grib2: bbox latitudes must satisfy -90 <= lat_min <= lat_max <= 90 (the order is "
				    "[lon_min, lat_min, lon_max, lat_max])");
			}
			result->mode = SpatialMode::BBOX;
			modes++;
		}
	}
	if (modes == 0) {
		throw InvalidInputException("read_grib2 needs one of points, bbox or all_cells := true: a single NBM CONUS "
		                            "message has 3.7 million cells");
	}
	if (modes > 1) {
		throw InvalidInputException("read_grib2: choose only one of points, bbox and all_cells");
	}

	if (wrap_given && result->mode != SpatialMode::BBOX) {
		throw InvalidInputException("read_grib2: wrap := true only applies to bbox");
	}
	if (result->mode == SpatialMode::BBOX && !result->bbox_wrap && result->bbox[2] < result->bbox[0]) {
		// lon_max < lon_min is a legitimate box across the antimeridian, but it is also what a swapped pair of
		// longitudes looks like, and the result (nearly every cell) is slow and wrong. Ask for it explicitly.
		double width = result->bbox[2] - result->bbox[0];
		width -= 360.0 * std::floor(width / 360.0);
		throw InvalidInputException("read_grib2: bbox lon_max (" + std::to_string(result->bbox[2]) +
		                            ") is less than lon_min (" + std::to_string(result->bbox[0]) +
		                            "), so the box would run east through the antimeridian and span " +
		                            std::to_string(width) +
		                            " degrees of longitude. The order is [lon_min, lat_min, lon_max, lat_max]; "
		                            "if you mean a box that crosses the antimeridian, add wrap := true");
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

//! Upper bound on the bytes of messages held in flight, so large messages cannot multiply memory use by the prefetch
//! depth. At least one message is always in flight, whatever its size.
constexpr int64_t PREFETCH_BYTE_BUDGET = 128LL * 1024 * 1024;

struct PendingFetch {
	std::future<std::string> bytes;
	int64_t length;
};

//! A file handle that is opened the first time a message is read from it, not when the read is queued. Messages are
//! queued ahead of the one being decoded, so opening at queue time would report a failure to open the next file against
//! the current message.
struct LazyReader {
	LazyReader(FileSystem &fs_p, std::string path_p) : fs(fs_p), path(std::move(path_p)) {
	}
	std::string ReadAt(int64_t offset, int64_t length) {
		if (!reader) {
			reader = make_uniq<FileReader>(fs, path);
		}
		return reader->ReadAt(offset, length);
	}

private:
	FileSystem &fs;
	std::string path;
	unique_ptr<FileReader> reader;
};

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
	//! The valid time ecCodes derives from each message (microseconds since the epoch), filled in as each is loaded.
	//! Used when the .idx gives no step, or there is no .idx.
	std::vector<optional<int64_t>> message_valid_times;
	//! Whether each message's reference time is a run time. If not, valid_time is NULL even when the .idx gave a step.
	std::vector<bool> reference_is_run_time;

	idx_t message_position = 0;
	FileSystem *fs = nullptr;
	//! Messages whose bytes are being fetched in the background, in message order. Fetching is waiting for round trips,
	//! so it overlaps with decoding; decoding itself stays on the scanning thread, which keeps row order deterministic.
	std::deque<PendingFetch> pending;
	int64_t pending_bytes = 0;
	idx_t prefetch_next = 0;
	//! std::launch::async when background threads are allowed (grib2_io_threads > 1); deferred (the default setting)
	//! runs each fetch on the scanning thread when it is needed
	std::launch fetch_policy = std::launch::deferred;
	//! The most recently used file, reused while consecutive messages come from it. Only used when fetches run on the
	//! scanning thread (deferred): messages are in file order, so one open handle is enough, and it saves a request per
	//! message on remote files (httpfs asks for the size every time a file is opened). Opened on first read.
	std::string reader_path;
	std::shared_ptr<LazyReader> reader;
	//! How many messages are fetched ahead of the one being decoded: grib2_io_threads, capped at
	//! GRIB2_MAX_PREFETCH_MESSAGES. Always at least one.
	idx_t prefetch_depth = 1;

	// The message currently being emitted
	bool loaded = false;
	idx_t cell_position = 0;
	idx_t cell_count = 0;
	std::unique_ptr<ProjectedGrid> grid;
	bool alternative_rows = false;
	DecodedValues field;

	// Cell selection, reused while consecutive messages share a grid
	optional<GridSpec> selection_spec;
	std::vector<PointHit> point_hits;
	std::vector<int64_t> box_cells;
};

unique_ptr<GlobalTableFunctionState> Grib2ReadInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<Grib2ReadBindData>();
	auto state = make_uniq<Grib2ReadGlobalState>();
	auto &fs = FileSystem::GetFileSystem(context);
	auto files = fs.GlobFiles(bind_data.pattern, FileGlobOptions::DISALLOW_EMPTY);
	std::sort(files.begin(), files.end(), [](const OpenFileInfo &a, const OpenFileInfo &b) { return a.path < b.path; });

	state->fs = &fs;
	const idx_t io_threads = Grib2IoThreads(context);
	state->fetch_policy = io_threads > 1 ? std::launch::async : std::launch::deferred;
	state->prefetch_depth = MinValue<idx_t>(io_threads, static_cast<idx_t>(GRIB2_MAX_PREFETCH_MESSAGES));

	unique_ptr<duckdb_re2::RE2> search;
	if (bind_data.has_search) {
		search = CompileSearch(bind_data.search);
	}
	std::vector<std::string> paths;
	for (const auto &file : files) {
		paths.push_back(file.path);
	}
	// Each message is validated as it is read, so the up-front check of the first message is skipped. Files are
	// indexed several at a time; the results come back in path order.
	const std::unordered_set<int64_t> wanted(bind_data.messages.begin(), bind_data.messages.end());
	for (auto &rows : IndexFiles(fs, paths, true, false, false, Grib2IoThreads(context), &context)) {
		for (auto &row : rows) {
			if (!wanted.empty() && wanted.find(row.message_number) == wanted.end()) {
				continue;
			}
			if (bind_data.has_search) {
				if (!row.inventory) {
					throw InvalidInputException("read_grib2: search needs the .idx file that goes with " +
					                            row.file_path +
					                            " (it holds the variable and level names); select messages with "
					                            "message := ... instead");
				}
				// Match the text as it is and with colons around it, so ":TMP:2 m above ground:" works too
				if (!duckdb_re2::RE2::PartialMatch(duckdb_re2::StringPiece(*row.inventory), *search) &&
				    !duckdb_re2::RE2::PartialMatch(duckdb_re2::StringPiece(":" + *row.inventory + ":"), *search)) {
					continue;
				}
			}
			if (row.byte_length > MAX_MESSAGE_BYTES) {
				throw InvalidInputException(
				    "read_grib2: " + row.file_path + ": message " + std::to_string(row.message_number) + " is " +
				    std::to_string(row.byte_length) + " bytes, above the limit of " +
				    std::to_string(MAX_MESSAGE_BYTES) + " (does the .idx belong to this file?)");
			}
			state->messages.push_back(std::move(row));
		}
	}
	state->short_names.resize(state->messages.size());
	state->message_valid_times.resize(state->messages.size());
	state->reference_is_run_time.assign(state->messages.size(), true);
	return state;
}

//! Start fetching the bytes of upcoming messages, keeping a few in flight
void FillPrefetch(Grib2ReadGlobalState &state) {
	while (state.pending.size() < state.prefetch_depth && state.prefetch_next < state.messages.size()) {
		const Grib2IndexRow &row = state.messages[state.prefetch_next];
		if (!state.pending.empty() && state.pending_bytes + row.byte_length > PREFETCH_BYTE_BUDGET) {
			break;
		}
		state.prefetch_next++;
		FileSystem *fs = state.fs;
		PendingFetch fetch;
		fetch.length = row.byte_length;
		std::shared_ptr<LazyReader> shared;
		if (state.fetch_policy == std::launch::deferred) {
			// Every fetch runs on this thread, so they can share one handle
			if (!state.reader || state.reader_path != row.file_path) {
				state.reader = std::make_shared<LazyReader>(*fs, row.file_path);
				state.reader_path = row.file_path;
			}
			shared = state.reader;
		}
		fetch.bytes = std::async(state.fetch_policy, [fs, shared, path = row.file_path, offset = row.byte_offset,
		                                              length = row.byte_length]() {
			if (shared) {
				return shared->ReadAt(offset, length);
			}
			// Background threads each open their own handle: concurrent reads on one remote handle are not something
			// to rely on
			const FileReader reader(*fs, path);
			return reader.ReadAt(offset, length);
		});
		state.pending_bytes += row.byte_length;
		state.pending.push_back(std::move(fetch));
	}
}

//! Read and decode the next message and work out which cells to emit
void LoadMessage(ClientContext &context, const Grib2ReadBindData &bind_data, Grib2ReadGlobalState &state) {
	const char *function = "read_grib2";
	const Grib2IndexRow &row = state.messages[state.message_position];
	try {
		// Decoding is the slow step; give a cancelled query the chance to stop before the next one starts
		context.InterruptCheck();
		FillPrefetch(state);
		const std::string bytes = state.pending.front().bytes.get();
		state.pending_bytes -= state.pending.front().length;
		state.pending.pop_front();
		FillPrefetch(state);
		// Without the up-front check, this is what notices an .idx that does not belong to the file
		MessageSummary indicator;
		size_t needed = 0;
		std::string indicator_error;
		if (ParseIndicator(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), indicator, needed,
		                   indicator_error) != SectionParseResult::OK) {
			if (indicator_error.empty()) {
				// A buffer too short for an indicator section is reported as "need more", with no text
				indicator_error =
				    "the message is " + std::to_string(bytes.size()) + " bytes, too short for a GRIB indicator section";
			}
			throw InvalidInputException(indicator_error + " (does the .idx belong to this file?)");
		}
		if (static_cast<int64_t>(indicator.total_length) != row.byte_length) {
			throw InvalidInputException("the message is " + std::to_string(indicator.total_length) +
			                            " bytes but the index says " + std::to_string(row.byte_length) +
			                            " (does the .idx belong to this file?)");
		}
		// The .idx supplies the labels and valid_time, and equal lengths do not prove it describes this file (a rerun
		// can produce messages of the same size), so check the reference time against the message's own
		if (row.inventory) {
			MessageSummary summary;
			std::string summary_error;
			if (ParseMessageSections(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), summary, needed,
			                         summary_error) == SectionParseResult::OK &&
			    !CheckIdxReferenceTime(summary, row.reference_time, summary_error)) {
				throw InvalidInputException(summary_error);
			}
		}
		Grib2Grid grib_grid;
		{
			// Only the ecCodes work is inside the guard: not the file read above or the cell selection below
			const Grib2EccodesGuard ecc_guard;
			CodesHandlePtr handle = OpenGribMessage(function, bytes.data(), bytes.size());
			codes_handle *h = handle.get();

			grib_grid = MakeGrid(h, function);
			state.field = DecodeValues(h, function, grib_grid.grid->Ni() * grib_grid.grid->Nj());
			state.short_names[state.message_position] = Grib2OptionalString(h, "shortName");
			const bool run_time = Grib2ReferenceTimeIsRunTime(h);
			state.reference_is_run_time[state.message_position] = run_time;
			if (run_time) {
				state.message_valid_times[state.message_position] = Grib2ValidTimeMicros(h);
			}
		}
		const int64_t cell_total = grib_grid.grid->Ni() * grib_grid.grid->Nj();
		state.alternative_rows = grib_grid.spec.alternative_rows;

		// Cell selection depends only on the grid, so keep it while messages share one
		const bool same_grid = state.selection_spec && *state.selection_spec == grib_grid.spec;
		state.grid = std::move(grib_grid.grid);
		if (!same_grid) {
			state.selection_spec = grib_grid.spec;
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
	} catch (const std::bad_alloc &) {
		// Not an input problem: a large message or grid did not fit in memory
		throw Exception(ExceptionType::OUT_OF_MEMORY, row.file_path + ": message " +
		                                                  std::to_string(row.message_number) +
		                                                  ": out of memory while reading or decoding the message");
	} catch (std::exception &error) {
		// what() of a DuckDB exception is JSON; the plain message reads better
		// Keep the original kind of error (IO, out of memory, interrupt, not implemented): everything arriving here
		// used to be reported as invalid input, which hid real failures such as a dropped connection. Errors
		// without a DuckDB type (INVALID) are still reported as invalid input.
		const ErrorData details(error);
		const ExceptionType type =
		    details.Type() == ExceptionType::INVALID ? ExceptionType::INVALID_INPUT : details.Type();
		throw Exception(type, row.file_path + ": message " + std::to_string(row.message_number) + ": " +
		                          details.RawMessage());
	}
}

ResultRow MakeRow(const Grib2ReadBindData &bind_data, const Grib2ReadGlobalState &state, idx_t cell_index) {
	ResultRow out;
	out.message_index = state.message_position;
	int64_t i = 0;
	int64_t j = 0;
	const int64_t ni = state.grid->Ni();
	if (bind_data.mode == SpatialMode::POINTS) {
		const PointHit &hit = state.point_hits[cell_index];
		i = hit.cell.i;
		j = hit.cell.j;
		out.lat = hit.cell.lat;
		out.lon = hit.cell.lon;
		out.query_lat = bind_data.points[hit.point_index].lat;
		out.query_lon = bind_data.points[hit.point_index].lon;
		out.distance_km = hit.cell.distance_km;
	} else {
		// A box lists its cells; the whole grid is every index in order
		const int64_t flat =
		    bind_data.mode == SpatialMode::BBOX ? state.box_cells[cell_index] : static_cast<int64_t>(cell_index);
		i = flat % ni;
		j = flat / ni;
		state.grid->Inverse(static_cast<double>(i), static_cast<double>(j), out.lat, out.lon);
	}
	out.i = i;
	out.j = j;
	out.value = state.field.At(RawValueIndex(ni, i, j, state.alternative_rows));
	return out;
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
			state.field = DecodedValues();
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
			if (!state.reference_is_run_time[row.message_index]) {
				// The reference time is a verifying or observation time, so adding a forecast time to it is wrong
				valid_time.WriteNull();
			} else if (message.reference_time && message.step_hours) {
				// The inventory's step, when it has one (this is what step_hours reports)
				valid_time.WriteValue(timestamp_t(message.reference_time->value + *message.step_hours * 3600000000LL));
			} else if (state.message_valid_times[row.message_index]) {
				// Otherwise the time ecCodes works out from the message: no .idx, or a step the inventory text cannot
				// give
				valid_time.WriteValue(timestamp_t(*state.message_valid_times[row.message_index]));
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
	// The path is declared positional-only with a real name (it used to be the auto-named col0) so that
	// duckdb_functions() reports it; it is still passed positionally.
	TableFunction function("read_grib2", vector<LogicalType> {}, Grib2ReadScan, Grib2ReadBind, Grib2ReadInit);
	auto &signature = function.GetSignature();
	signature.AddPositionalOnly("path", LogicalType::VARCHAR);
	// A keyword-only parameter without a default is required, so the optional ones default to NULL
	signature.AddKeywordOnly("message", LogicalType::ANY, Value());
	signature.AddKeywordOnly("search", LogicalType::VARCHAR, Value(LogicalType::VARCHAR));
	signature.AddKeywordOnly("points", LogicalType::ANY, Value());
	signature.AddKeywordOnly("bbox", LogicalType::ANY, Value());
	signature.AddKeywordOnly("all_cells", LogicalType::BOOLEAN, Value::BOOLEAN(false));
	signature.AddKeywordOnly("wrap", LogicalType::BOOLEAN, Value::BOOLEAN(false));

	// The bare loader.RegisterFunction(TableFunction) overload cannot carry documentation. The CreateInfo form can, but
	// it must ask for ALTER_ON_CONFLICT itself: that is what the bare overload sets, while the default is
	// ERROR_ON_CONFLICT.
	CreateTableFunctionInfo info(std::move(function));
	info.on_conflict = OnCreateConflict::ALTER_ON_CONFLICT;

	FunctionDescription description;
	description.parameter_types = {LogicalType::VARCHAR, LogicalType::ANY,     LogicalType::VARCHAR, LogicalType::ANY,
	                               LogicalType::ANY,     LogicalType::BOOLEAN, LogicalType::BOOLEAN};
	description.parameter_names = {"path", "message", "search", "points", "bbox", "all_cells", "wrap"};
	description.description = "Decodes GRIB2 messages into one row per selected cell, at the nearest grid cell to each "
	                          "point, inside a bounding box, or across the whole grid, with cells masked by a bitmap "
	                          "returned as NULL.";
	description.examples.push_back("SELECT * FROM read_grib2('path/to/file.grib2', search := 'TMP:2 m above ground', "
	                               "points := [{'lat': 37.21, 'lon': -93.29}])");
	description.categories.push_back("grib2");
	info.descriptions.push_back(std::move(description));

	loader.RegisterFunction(std::move(info));
}

} // namespace duckdb
