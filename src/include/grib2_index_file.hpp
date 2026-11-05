#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"

#include <memory>
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

	//! Open a file, or return nullptr when it does not exist. One request for a remote file, where checking that it
	//! exists first and then opening it would be two.
	static unique_ptr<FileReader> OpenIfExists(FileSystem &fs, const std::string &path) {
		auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
		if (!handle) {
			return nullptr;
		}
		return unique_ptr<FileReader>(new FileReader(path, std::move(handle)));
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

private:
	FileReader(const std::string &path_p, unique_ptr<FileHandle> handle_p)
	    : path(path_p), handle(std::move(handle_p)), size(static_cast<int64_t>(handle->GetFileSize())) {
	}
};

optional<timestamp_t> MakeTimestamp(int year, int month, int day, int hour, int minute, int second);

//! Index every message in one file, appending to `rows`. Uses the ".idx" sidecar when use_idx is true and one exists,
//! otherwise hops through the file using the length in each indicator section. With `details`, also reads a short
//! prefix of each message and asks ecCodes for names, level and grid details.
//!
//! verify_first_message checks the first message against the sidecar (every message when `details` is set), which
//! costs one read; callers that validate each message as they read it can turn it off.
void IndexFile(FileSystem &fs, const std::string &path, bool use_idx, bool details, std::vector<Grib2IndexRow> &rows,
               bool verify_first_message = true);

//! Index several files, up to max_parallel at a time. Reading the index of a remote file is mostly waiting for round
//! trips, so doing several at once is much faster than one after another. The result has one entry per path, in the
//! order of `paths`, whatever order the work finished in. An error from any file is rethrown after all have finished.
//! With `details` the files are done one at a time, because that path calls into ecCodes.
std::vector<std::vector<Grib2IndexRow>> IndexFiles(FileSystem &fs, const std::vector<std::string> &paths, bool use_idx,
                                                   bool details, bool verify_first_message, idx_t max_parallel);

} // namespace duckdb
