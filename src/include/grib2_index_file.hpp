#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"

#include <string>
#include <vector>

namespace duckdb {

//! One row of the message index: where a GRIB message is and what it contains. Which fields are set depends on how
//! the index was built (see IndexFile); the rest stay empty.
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

optional<timestamp_t> MakeTimestamp(int year, int month, int day, int hour, int minute, int second);

//! Index every message in one file, appending to `rows`. Uses the ".idx" sidecar when use_idx is true and one exists,
//! otherwise hops through the file using the length in each indicator section. With `details`, also reads a short
//! prefix of each message and asks ecCodes for names, level and grid details.
void IndexFile(FileSystem &fs, const std::string &path, bool use_idx, bool details, std::vector<Grib2IndexRow> &rows);

} // namespace duckdb
