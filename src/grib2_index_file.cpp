//! GRIB2 file indexing and metadata extraction.
//!
//! This file builds a safe catalog for one or more GRIB2 files by locating each message, validating its headers, and
//! extracting enough metadata to identify the payload without decoding every value. The result is a vector of
//! Grib2IndexRow entries describing where each message lives in the file and what it contains. If a .idx sidecar is
//! available it is parsed and validated; otherwise the code walks the file from indicator section to indicator section.
//! When details are requested, a short prefix of each message is read and ecCodes is used to recover names, levels and
//! grid metadata. The logic is intentionally defensive: bounds checks, limit checks and message-length validation keep
//! malformed or hostile inputs from causing invalid reads or resource abuse.

#include "grib2_index_file.hpp"

#include "grib2_codes.hpp"
#include "grib2_idx.hpp"
#include "grib2_message.hpp"
#include "grib2_valid_time.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/time.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace duckdb {

//! The indicator section (section 0) alone is 16 bytes, so fewer than this cannot be a message
static constexpr int64_t MIN_GRIB_MESSAGE_BYTES = 16;

optional<timestamp_t> MakeTimestamp(int year, int month, int day, int hour, int minute, int second) {
	date_t date;
	if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59 ||
	    !Date::TryFromDate(year, month, day, date)) {
		return optional<timestamp_t>();
	}
	return Timestamp::FromDatetime(date, Time::FromTime(hour, minute, second));
}

bool CheckIdxReferenceTime(const MessageSummary &summary, const optional<timestamp_t> &idx_reference_time,
                           std::string &error) {
	// The .idx records the time to the minute, so seconds in section 1 are left out of the comparison
	const optional<timestamp_t> message_time =
	    MakeTimestamp(summary.year, summary.month, summary.day, summary.hour, summary.minute, 0);
	if (!idx_reference_time || !message_time || idx_reference_time->value == message_time->value) {
		return true;
	}
	error = "the message's reference time is " + Timestamp::ToString(*message_time) + " but the .idx says " +
	        Timestamp::ToString(*idx_reference_time) + " (does the .idx belong to this file?)";
	return false;
}

//! Read the section headers of the message at `offset`. Starts with a short prefix and grows it as the parser asks.
//! known_length is the message length when it is already known, or -1. On success `prefix` holds the bytes read.
static bool ReadSections(const FileReader &file, int64_t offset, int64_t known_length, MessageSummary &summary,
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
		// The section lengths are the file's own claim; refuse before allocating or fetching that much
		if (!HeaderSizeAcceptable(needed, error)) {
			return false;
		}
		want = std::min<int64_t>(static_cast<int64_t>(needed), limit);
	}
	error = "could not read the message headers";
	return false;
}

static void FillFromSections(Grib2IndexRow &row, const MessageSummary &summary, bool set_reference_time) {
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

//! Names, level and grid details from ecCodes, using only the short prefix already read (sections 0 to 5).
//! ReadSections has already refused headers over MAX_HEADER_BYTES. The prefix reaches ecCodes without the other checks
//! the decode path applies, so a message that claims more points than the extension will ever decode gets no details:
//! its row is still listed.
static void FillDetails(Grib2IndexRow &row, const MessageSummary &summary, const std::string &prefix) {
	std::string unused_error;
	if (!CheckMessageCounts(summary, unused_error) || static_cast<int64_t>(prefix.size()) > MAX_HEADER_BYTES) {
		return;
	}
	const Grib2EccodesGuard ecc_guard;
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
	// valid_time: the inventory's step when it gave one, otherwise the time ecCodes works out from the message. A
	// reference time that is not a run time gives no valid time, whichever way it was worked out.
	if (!Grib2ReferenceTimeIsRunTime(h)) {
		row.valid_time.reset();
	} else if (!row.valid_time) {
		const std::optional<int64_t> micros = Grib2ValidTimeMicros(h);
		if (micros) {
			row.valid_time = timestamp_t(*micros);
		}
	}
}

[[noreturn]] static void ThrowBadMessage(const std::string &path, int64_t message_number, int64_t offset,
                                         const std::string &error) {
	throw InvalidInputException(path + ": message " + std::to_string(message_number) + " at byte offset " +
	                            std::to_string(offset) + ": " + error);
}

void IndexFile(FileSystem &fs, const std::string &path, bool use_idx, bool details, std::vector<Grib2IndexRow> &rows,
               bool verify_ends, ClientContext *context) {
	const FileReader file(fs, path);
	const std::string idx_path = path + ".idx";
	// A zero-byte file is not a GRIB file with no messages: it is usually a download that failed, and an empty result
	// would hide that
	if (file.size == 0) {
		throw InvalidInputException(path + ": the file is empty");
	}

	// Opening the sidecar directly, instead of checking that it exists first, saves a request on remote files
	const unique_ptr<FileReader> idx_file = use_idx ? FileReader::OpenIfExists(fs, idx_path) : nullptr;
	if (idx_file) {
		std::vector<IdxEntry> entries;
		std::string error;
		if (idx_file->size > MAX_IDX_BYTES) {
			throw InvalidInputException(idx_path + ": the file is " + std::to_string(idx_file->size) +
			                            " bytes, which is too large for an .idx inventory (limit " +
			                            std::to_string(MAX_IDX_BYTES) + ")");
		}
		if (!ParseIdx(idx_file->ReadAt(0, idx_file->size), file.size, entries, error)) {
			throw InvalidInputException(idx_path + ": " + error);
		}
		for (size_t i = 0; i < entries.size(); i++) {
			if (context) {
				context->InterruptCheck();
			}
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
			if (row.reference_time && row.step_hours) {
				row.valid_time = timestamp_t(row.reference_time->value + *row.step_hours * 3600000000LL);
			}
			row.percentile = entry.percentile;
			row.threshold_lower = entry.threshold_lower;
			row.threshold_upper = entry.threshold_upper;

			// A sidecar that does not belong to the file would silently give wrong offsets, so check it against the
			// messages themselves: every message with details, otherwise the first and the last. The last message's
			// length is the rest of the file, so it also catches a file that has grown or shrunk since the sidecar was
			// written. A wrong entry in between is only found when that message is read (read_grib2 always does).
			if (details || (verify_ends && (i == 0 || i + 1 == entries.size()))) {
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
					FillDetails(row, summary, prefix);
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
		if (context) {
			context->InterruptCheck();
		}
		// Bytes left over after the last message (a stray newline, padding) are too few to hold another message. Say
		// so, rather than reporting a truncated message that does not exist. (Not for the first message: a file that is
		// short from the start is a truncated file.)
		if (number > 1 && file.size - offset < MIN_GRIB_MESSAGE_BYTES) {
			ThrowBadMessage(path, number, offset,
			                std::to_string(file.size - offset) +
			                    " byte(s) follow the last message, too few for another GRIB message (trailing data?)");
		}
		MessageSummary summary;
		std::string prefix;
		std::string error;
		if (!ReadSections(file, offset, -1, summary, prefix, error)) {
			ThrowBadMessage(path, number, offset, error);
		}
		int64_t length = 0;
		if (!MessageLengthWithinFile(summary.total_length, offset, file.size, length, error)) {
			ThrowBadMessage(path, number, offset, error);
		}
		Grib2IndexRow row;
		row.file_path = path;
		row.message_number = number;
		row.byte_offset = offset;
		row.byte_length = length;
		FillFromSections(row, summary, true);
		if (details) {
			FillDetails(row, summary, prefix);
		}
		rows.push_back(std::move(row));
		offset += length;
		number++;
	}
}

std::vector<std::vector<Grib2IndexRow>> IndexFiles(FileSystem &fs, const std::vector<std::string> &paths, bool use_idx,
                                                   bool details, bool verify_ends, idx_t max_parallel,
                                                   ClientContext *context) {
	std::vector<std::vector<Grib2IndexRow>> results(paths.size());
	std::vector<std::exception_ptr> errors(paths.size());
	std::atomic<size_t> next(0);
	const auto work = [&]() {
		while (true) {
			const size_t i = next.fetch_add(1);
			if (i >= paths.size()) {
				return;
			}
			try {
				IndexFile(fs, paths[i], use_idx, details, results[i], verify_ends, context);
			} catch (...) {
				errors[i] = std::current_exception();
			}
		}
	};

	const size_t workers = details ? 1 : std::max<size_t>(1, std::min<size_t>(max_parallel, paths.size()));
	// The calling thread takes part, so one fewer thread is started. If the system refuses to start a thread (limits,
	// memory), carry on with the threads that did start rather than letting std::system_error escape while joinable
	// threads are alive, which would call std::terminate.
	std::vector<std::thread> threads;
	for (size_t t = 1; t < workers; t++) {
		try {
			threads.emplace_back(work);
		} catch (const std::system_error &) {
			break;
		}
	}
	work();
	for (auto &thread : threads) {
		thread.join();
	}
	for (const auto &error : errors) {
		if (error) {
			std::rethrow_exception(error);
		}
	}
	return results;
}

} // namespace duckdb
