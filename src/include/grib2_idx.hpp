#pragma once

// Parser for wgrib2-style ".idx" inventory files such as the ones NOAA publishes next to NBM and HRRR GRIB2 files.
//
// A line looks like
//   133:123034124:d=2026102300:TMP:2 m above ground:1 hour fcst:
// that is: message number, byte offset, reference time, variable, level, forecast, and an optional qualifier. The
// qualifier is free text such as "50% level", "prob >12.7:prob fcst 255/255" or "ens std dev". It can contain colons
// and the line does not always end with a colon, so only the first six fields are split on colons and the rest of the
// line is the qualifier.
//
// The text of a line is not a unique identifier: several messages can share a variable, level, forecast and
// qualifier. The message number (and byte offset) is the only reliable key.
//
// Deliberately free of DuckDB and ecCodes dependencies so it can be unit tested on its own
// (see test/cpp/idx_check.cpp).

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace duckdb {

struct IdxEntry {
	int64_t message_number = 0;
	int64_t byte_offset = 0;
	//! Length of the message in bytes, or -1 when unknown (the last message when no file size was given)
	int64_t byte_length = -1;
	int year = 0;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	std::string variable;
	std::string level;
	//! Raw forecast text, for example "1 hour fcst", "anl" or "0-1 hour acc fcst"
	std::string forecast;
	//! Raw qualifier text, empty when the line has none
	std::string qualifier;

	//! Forecast hour when the forecast text is "anl" (0) or "N hour fcst"; empty for accumulations, maxima, etc.
	std::optional<int64_t> step_hours;
	//! N for a qualifier of the form "N% level"
	std::optional<double> percentile;
	//! Bounds of a probability qualifier: "prob >N" sets lower, "prob <N" sets upper, "prob >=A <B" sets both.
	//! The raw qualifier keeps whether a bound is inclusive.
	std::optional<double> threshold_lower;
	std::optional<double> threshold_upper;

	//! "variable:level:forecast", followed by ":qualifier" when there is one. Intended for searching.
	std::string Inventory() const {
		std::string out = variable + ":" + level + ":" + forecast;
		if (!qualifier.empty()) {
			out += ":" + qualifier;
		}
		return out;
	}
};

inline bool IdxParseInt(const std::string &text, int64_t &value) {
	if (text.empty()) {
		return false;
	}
	char *end = nullptr;
	errno = 0;
	const long long parsed = std::strtoll(text.c_str(), &end, 10);
	if (errno != 0 || end != text.c_str() + text.size()) {
		return false;
	}
	value = static_cast<int64_t>(parsed);
	return true;
}

inline bool IdxParseDouble(const std::string &text, double &value) {
	if (text.empty()) {
		return false;
	}
	char *end = nullptr;
	errno = 0;
	const double parsed = std::strtod(text.c_str(), &end);
	if (errno != 0 || end != text.c_str() + text.size()) {
		return false;
	}
	value = parsed;
	return true;
}

//! Derive step_hours, percentile and the probability thresholds from the raw text.
inline void IdxAnalyze(IdxEntry &entry) {
	entry.step_hours.reset();
	entry.percentile.reset();
	entry.threshold_lower.reset();
	entry.threshold_upper.reset();

	int64_t number = 0;
	const std::string hour_suffix = " hour fcst";
	if (entry.forecast == "anl") {
		entry.step_hours = 0;
	} else if (entry.forecast.size() > hour_suffix.size() &&
	           entry.forecast.compare(entry.forecast.size() - hour_suffix.size(), hour_suffix.size(), hour_suffix) ==
	               0 &&
	           IdxParseInt(entry.forecast.substr(0, entry.forecast.size() - hour_suffix.size()), number)) {
		entry.step_hours = number;
	}

	double value = 0;
	const std::string percentile_suffix = "% level";
	const std::string &q = entry.qualifier;
	if (q.size() > percentile_suffix.size() &&
	    q.compare(q.size() - percentile_suffix.size(), percentile_suffix.size(), percentile_suffix) == 0 &&
	    IdxParseDouble(q.substr(0, q.size() - percentile_suffix.size()), value)) {
		entry.percentile = value;
	}

	if (q.compare(0, 5, "prob ") == 0) {
		// The threshold expression runs up to the first colon: ">50", "<304.8" or ">=1 <2"
		const size_t colon = q.find(':', 5);
		const std::string expression = q.substr(5, colon == std::string::npos ? std::string::npos : colon - 5);
		std::optional<double> lower;
		std::optional<double> upper;
		bool valid = true;
		size_t start = 0;
		while (valid && start < expression.size()) {
			size_t end = expression.find(' ', start);
			if (end == std::string::npos) {
				end = expression.size();
			}
			const std::string token = expression.substr(start, end - start);
			start = end + 1;
			if (token.empty()) {
				continue;
			}
			if (token[0] != '>' && token[0] != '<') {
				valid = false;
				break;
			}
			size_t digits = 1;
			if (digits < token.size() && token[digits] == '=') {
				digits++;
			}
			if (!IdxParseDouble(token.substr(digits), value)) {
				valid = false;
				break;
			}
			if (token[0] == '>') {
				lower = value;
			} else {
				upper = value;
			}
		}
		if (valid && (lower || upper)) {
			entry.threshold_lower = lower;
			entry.threshold_upper = upper;
		}
	}
}

//! Parse one line. Returns false and sets error when the line is malformed. byte_length is left at -1.
inline bool ParseIdxLine(const std::string &raw_line, IdxEntry &out, std::string &error) {
	std::string line = raw_line;
	while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
		line.pop_back();
	}

	std::string fields[6];
	size_t start = 0;
	for (int f = 0; f < 6; f++) {
		const size_t colon = line.find(':', start);
		if (colon == std::string::npos) {
			if (f < 5) {
				error = "expected at least 6 colon-separated fields";
				return false;
			}
			// A forecast with no trailing colon and no qualifier
			fields[5] = line.substr(start);
			start = line.size();
		} else {
			fields[f] = line.substr(start, colon - start);
			start = colon + 1;
		}
	}

	IdxEntry entry;
	if (fields[0].find('.') != std::string::npos) {
		error = "message numbers of the form n.m (several fields in one message) are not supported yet";
		return false;
	}
	if (!IdxParseInt(fields[0], entry.message_number) || entry.message_number < 1) {
		error = "invalid message number '" + fields[0] + "'";
		return false;
	}
	if (!IdxParseInt(fields[1], entry.byte_offset) || entry.byte_offset < 0) {
		error = "invalid byte offset '" + fields[1] + "'";
		return false;
	}

	const std::string &date = fields[2];
	if (date.compare(0, 2, "d=") != 0 || (date.size() != 10 && date.size() != 12 && date.size() != 14)) {
		error = "invalid reference time '" + date + "' (expected d=YYYYMMDDHH)";
		return false;
	}
	int64_t year = 0, month = 0, day = 0, hour = 0, minute = 0;
	if (!IdxParseInt(date.substr(2, 4), year) || !IdxParseInt(date.substr(6, 2), month) ||
	    !IdxParseInt(date.substr(8, 2), day) || (date.size() >= 12 && !IdxParseInt(date.substr(10, 2), hour)) ||
	    (date.size() >= 14 && !IdxParseInt(date.substr(12, 2), minute)) || month < 1 || month > 12 || day < 1 ||
	    day > 31 || hour > 23 || minute > 59) {
		error = "invalid reference time '" + date + "'";
		return false;
	}
	entry.year = static_cast<int>(year);
	entry.month = static_cast<int>(month);
	entry.day = static_cast<int>(day);
	entry.hour = static_cast<int>(hour);
	entry.minute = static_cast<int>(minute);

	entry.variable = fields[3];
	entry.level = fields[4];
	entry.forecast = fields[5];
	entry.qualifier = start <= line.size() ? line.substr(start) : "";
	IdxAnalyze(entry);
	out = entry;
	return true;
}

//! Parse a whole .idx file. Byte lengths come from the distance to the next offset; the last message's length needs
//! file_size (pass -1 when unknown and it stays -1). Blank lines are ignored.
inline bool ParseIdx(const std::string &text, int64_t file_size, std::vector<IdxEntry> &out, std::string &error) {
	std::vector<IdxEntry> entries;
	size_t line_number = 0;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) {
			end = text.size();
		}
		std::string line = text.substr(start, end - start);
		start = end + 1;
		line_number++;
		while (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if (line.empty()) {
			continue;
		}
		IdxEntry entry;
		std::string line_error;
		if (!ParseIdxLine(line, entry, line_error)) {
			error = "line " + std::to_string(line_number) + ": " + line_error;
			return false;
		}
		if (!entries.empty() && entry.byte_offset <= entries.back().byte_offset) {
			error = "line " + std::to_string(line_number) + ": byte offset " + std::to_string(entry.byte_offset) +
			        " is not greater than the previous offset";
			return false;
		}
		entries.push_back(entry);
	}
	for (size_t i = 0; i + 1 < entries.size(); i++) {
		entries[i].byte_length = entries[i + 1].byte_offset - entries[i].byte_offset;
	}
	if (!entries.empty() && file_size >= 0) {
		if (file_size <= entries.back().byte_offset) {
			error = "file size " + std::to_string(file_size) + " is not greater than the last byte offset " +
			        std::to_string(entries.back().byte_offset);
			return false;
		}
		entries.back().byte_length = file_size - entries.back().byte_offset;
	}
	out = std::move(entries);
	return true;
}

} // namespace duckdb
