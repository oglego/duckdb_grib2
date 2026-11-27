#pragma once

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"

#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {

//! Largest .idx sidecar that is read. A real inventory is tens of kilobytes (about 150 bytes a message); the whole file
//! is held in memory, so a much larger one is a mistake or an attack, not an inventory.
constexpr int64_t MAX_IDX_BYTES = 64LL * 1024 * 1024;
//! Largest single message that is fetched and decoded. NBM and HRRR messages are a few megabytes at most; this leaves a
//! wide margin while stopping a corrupt index or message length from asking for gigabytes at once. ecCodes copies the
//! message, so a message at the limit costs about twice this in memory before any value is decoded.
constexpr int64_t MAX_MESSAGE_BYTES = 256LL * 1024 * 1024;

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
	//! reference_time + step_hours when the inventory has a step; with details, otherwise the time ecCodes works out
	//! from the message (nothing when the reference time is not a run time, see Grib2ReferenceTimeIsRunTime)
	optional<timestamp_t> valid_time;
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
	      size(SafeFileSize(path_p, handle->GetFileSize())) {
	}

	//! Open a file that may not be there, such as the .idx next to a GRIB file. A missing file gives nullptr. This is
	//! one request for a remote file, where checking that it exists first and then opening it would be two.
	//!
	//! httpfs reports a missing object as nullptr only for HTTP 404. S3 answers 403 instead of 404 for a key that does
	//! not exist when the caller may not list the bucket, which is how most private buckets are set up, so a plain
	//! file with no .idx would fail with an access error. The GRIB file itself has already been opened with the same
	//! credentials by then, so a 403 on its sidecar is read as "no sidecar". Other failures, 401 included (the
	//! credentials are wrong), are still reported.
	static unique_ptr<FileReader> OpenIfExists(FileSystem &fs, const std::string &path) {
		unique_ptr<FileHandle> handle;
		try {
			handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_NULL_IF_NOT_EXISTS);
		} catch (std::exception &error) {
			const ErrorData details(error);
			const auto status = details.ExtraInfo().find("status_code");
			if (status != details.ExtraInfo().end() && status->second == "403") {
				return nullptr;
			}
			throw;
		}
		if (!handle) {
			return nullptr;
		}
		return unique_ptr<FileReader>(new FileReader(path, std::move(handle)));
	}

	std::string ReadAt(int64_t offset, int64_t length) const {
		// Written as a subtraction so that a huge length cannot overflow offset + length
		if (offset < 0 || length < 0 || offset > size || length > size - offset) {
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
	    : path(path_p), handle(std::move(handle_p)), size(SafeFileSize(path_p, handle->GetFileSize())) {
	}

	static int64_t SafeFileSize(const std::string &path_p, idx_t file_size) {
		if (file_size > static_cast<idx_t>(std::numeric_limits<int64_t>::max())) {
			throw IOException(path_p + ": file is too large (" + std::to_string(file_size) + " bytes; the limit is " +
			                  std::to_string(std::numeric_limits<int64_t>::max()) + ")");
		}
		return static_cast<int64_t>(file_size);
	}
};

optional<timestamp_t> MakeTimestamp(int year, int month, int day, int hour, int minute, int second);

struct MessageSummary;

//! Check that the reference time an .idx entry gives (to the minute) is the one in the message's own section 1. The
//! .idx supplies the variable, level, forecast and valid_time, so a sidecar written for a different run of the same
//! file name would label the wrong data; matching message lengths alone do not rule that out (constant fields and
//! fixed-size packing give equal lengths from run to run). Returns false and sets `error` on a mismatch. Does nothing
//! when either time is unavailable.
bool CheckIdxReferenceTime(const MessageSummary &summary, const optional<timestamp_t> &idx_reference_time,
                           std::string &error);

//! Index every message in one file, appending to `rows`. Uses the ".idx" sidecar when use_idx is true and one exists,
//! otherwise hops through the file using the length in each indicator section. With `details`, also reads a short
//! prefix of each message and asks ecCodes for names, level and grid details.
//!
//! verify_ends checks the first and the last message against the sidecar (every message when `details` is set), which
//! costs two reads (the first and the last message catch a sidecar written for another version of the file, or a file
//! that changed size); callers that validate each message as they read it can turn it off.
//!
//! When `context` is given, a cancelled query stops between messages.
void IndexFile(FileSystem &fs, const std::string &path, bool use_idx, bool details, std::vector<Grib2IndexRow> &rows,
               bool verify_ends = true, ClientContext *context = nullptr);

//! Index several files, up to max_parallel at a time. Reading the index of a remote file is mostly waiting for round
//! trips, so doing several at once is much faster than one after another. The result has one entry per path, in the
//! order of `paths`, whatever order the work finished in. An error from any file is rethrown after all have finished.
//! With `details` the files are done one at a time, because that path calls into ecCodes. With max_parallel = 1 no
//! thread is started and everything runs on the calling thread. When `context` is given, a cancelled query stops
//! between messages.
std::vector<std::vector<Grib2IndexRow>> IndexFiles(FileSystem &fs, const std::vector<std::string> &paths, bool use_idx,
                                                   bool details, bool verify_ends, idx_t max_parallel,
                                                   ClientContext *context = nullptr);

} // namespace duckdb
