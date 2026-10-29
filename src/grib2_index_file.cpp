#include "grib2_index_file.hpp"

#include "grib2_codes.hpp"
#include "grib2_idx.hpp"
#include "grib2_message.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/time.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace duckdb {

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

//! Names, level and grid details from ecCodes, using only the short prefix already read
static void FillDetails(Grib2IndexRow &row, const std::string &prefix) {
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

[[noreturn]] static void ThrowBadMessage(const std::string &path, int64_t message_number, int64_t offset,
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

} // namespace duckdb
