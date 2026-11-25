// Checks Grib2ValidTimeMicros, the valid time read from the message, against valid times computed independently.
//
//  * the calendar arithmetic, against values computed with Python's calendar.timegm, including leap days, the 1900
//    non-leap century, the earliest and latest years a TIMESTAMP holds, and dates that do not exist
//  * the eight messages in test/data/valid_times.grib2 (minutes, days and 3-hour units, interval templates, a leap day,
//    a year boundary), against test/data/valid_times_expected.tsv
//  * that the extension's own message screen accepts every one of those messages
//  * that the same times come out of the short prefix (sections 0 to 5) that grib2_index(details := true) gives ecCodes
//  * the four messages in test/data/reference_significance.grib2: a reference time that is not a run time (code table
//    1.2 values 2 and 3) gets no valid time, however ecCodes would add the forecast time to it
//
// Build and run from the repository root (see .github/workflows/GridCheck.yml). Needs ecCodes, not DuckDB.

#include "grib2_message.hpp"
#include "grib2_valid_time.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

using namespace duckdb;

static int checks = 0;
static int failures = 0;

static void Check(bool ok, const std::string &what) {
	checks++;
	if (!ok) {
		failures++;
		std::cout << "FAIL: " << what << "\n";
	}
}

static void CheckMicros(int64_t y, int mo, int d, int h, int mi, int64_t expected) {
	const auto got = Grib2MakeMicros(y, mo, d, h, mi);
	std::ostringstream label;
	label << y << "-" << mo << "-" << d << " " << h << ":" << mi;
	Check(got.has_value() && *got == expected,
	      label.str() + " -> " + (got ? std::to_string(*got) : "nothing") + ", expected " + std::to_string(expected));
}

static void CheckRejected(int64_t y, int mo, int d, int h, int mi) {
	std::ostringstream label;
	label << y << "-" << mo << "-" << d << " " << h << ":" << mi << " should be rejected";
	Check(!Grib2MakeMicros(y, mo, d, h, mi).has_value(), label.str());
}

//! Sections 0 to 5 of a message: what grib2_index(details := true) hands to ecCodes
static std::vector<uint8_t> PrefixThroughSection5(const uint8_t *message, size_t length) {
	size_t pos = 16;
	while (pos + 5 <= length) {
		if (message[pos + 4] == 6) {
			break;
		}
		const uint32_t section_length = (uint32_t(message[pos]) << 24) | (uint32_t(message[pos + 1]) << 16) |
		                                (uint32_t(message[pos + 2]) << 8) | uint32_t(message[pos + 3]);
		if (section_length == 0) {
			break;
		}
		pos += section_length;
	}
	return std::vector<uint8_t>(message, message + std::min(pos, length));
}

//! Valid time and run-time flag through a partial (sections 0 to 5) handle, as FillDetails does
static std::optional<int64_t> ValidTimeFromPrefix(const uint8_t *message, size_t length, bool &run_time) {
	const std::vector<uint8_t> prefix = PrefixThroughSection5(message, length);
	codes_handle *h = codes_handle_new_from_partial_message_copy(nullptr, prefix.data(), prefix.size());
	if (!h) {
		run_time = false;
		return std::nullopt;
	}
	run_time = Grib2ReferenceTimeIsRunTime(h);
	const auto micros = Grib2ValidTimeMicros(h);
	codes_handle_delete(h);
	return micros;
}

int main() {
	CheckMicros(1970, 1, 1, 0, 0, 0);
	CheckMicros(1969, 12, 31, 23, 59, -60000000LL);
	CheckMicros(2000, 2, 29, 12, 30, 951827400000000LL); // 2000 is a leap year
	CheckMicros(1900, 3, 1, 0, 0, -2203891200000000LL);  // 1900 is not
	CheckMicros(1, 1, 1, 0, 0, -62135596800000000LL);
	CheckMicros(9999, 12, 31, 23, 59, 253402300740000000LL);
	CheckMicros(2026, 10, 23, 1, 0, 1792717200000000LL);
	CheckMicros(2100, 2, 28, 23, 59, 4107542340000000LL);
	CheckRejected(1900, 2, 29, 0, 0); // not a leap year
	CheckRejected(2100, 2, 29, 0, 0);
	CheckRejected(2026, 4, 31, 0, 0);
	CheckRejected(2026, 13, 1, 0, 0);
	CheckRejected(2026, 0, 1, 0, 0);
	CheckRejected(2026, 1, 0, 0, 0);
	CheckRejected(2026, 1, 1, 24, 0);
	CheckRejected(2026, 1, 1, 0, 60);
	CheckRejected(2026, 1, 1, -1, 0);
	CheckRejected(0, 1, 1, 0, 0);
	CheckRejected(10000, 1, 1, 0, 0);

	// The messages and their expected valid times
	std::ifstream tsv("test/data/valid_times_expected.tsv");
	std::vector<int64_t> expected;
	std::string line;
	std::getline(tsv, line); // header
	while (std::getline(tsv, line)) {
		const size_t last_tab = line.rfind('\t');
		Check(last_tab != std::string::npos, "malformed expectation line: " + line);
		if (last_tab != std::string::npos) {
			expected.push_back(std::stoll(line.substr(last_tab + 1)));
		}
	}
	Check(expected.size() == 8, "expected eight lines in valid_times_expected.tsv");

	std::ifstream in("test/data/valid_times.grib2", std::ios::binary);
	const std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	Check(!file.empty(), "cannot read test/data/valid_times.grib2");

	size_t pos = 0;
	size_t number = 0;
	while (pos + 16 <= file.size()) {
		uint64_t length = 0;
		for (int b = 0; b < 8; b++) {
			length = (length << 8) | file[pos + 8 + b];
		}
		Check(length >= 16 && pos + length <= file.size(),
		      "message " + std::to_string(number + 1) + " has a bad length");
		if (length < 16 || pos + length > file.size()) {
			break;
		}
		const std::string name = "message " + std::to_string(number + 1);
		std::string error;
		Check(ScreenMessage(file.data() + pos, static_cast<size_t>(length), error),
		      name + " was refused by the message screen: " + error);

		codes_handle *h = codes_handle_new_from_message_copy(nullptr, file.data() + pos, static_cast<size_t>(length));
		Check(h != nullptr, name + ": ecCodes could not open it");
		if (h) {
			const auto got = Grib2ValidTimeMicros(h);
			Check(number < expected.size() && got.has_value() && *got == expected[number],
			      name + ": valid time " + (got ? std::to_string(*got) : "nothing") + ", expected " +
			          (number < expected.size() ? std::to_string(expected[number]) : "no expectation"));
			Check(Grib2ReferenceTimeIsRunTime(h), name + ": a forecast reference time was not taken as a run time");
			codes_handle_delete(h);

			// The same answer from the prefix the index function uses
			bool prefix_run_time = false;
			const auto from_prefix =
			    ValidTimeFromPrefix(file.data() + pos, static_cast<size_t>(length), prefix_run_time);
			Check(prefix_run_time && from_prefix.has_value() && number < expected.size() &&
			          *from_prefix == expected[number],
			      name + ": the valid time from sections 0 to 5 differs from the full message");
		}
		pos += static_cast<size_t>(length);
		number++;
	}
	Check(number == 8, "found " + std::to_string(number) + " messages, expected eight");

	// Reference times that are not run times
	std::ifstream sig_tsv("test/data/reference_significance_expected.tsv");
	struct Expected {
		int significance;
		bool has_valid_time;
		std::string valid_time;
	};
	std::vector<Expected> sig_expected;
	std::getline(sig_tsv, line); // header
	while (std::getline(sig_tsv, line)) {
		std::istringstream fields(line);
		std::string n, sig, valid;
		std::getline(fields, n, '\t');
		std::getline(fields, sig, '\t');
		std::getline(fields, valid, '\t');
		sig_expected.push_back({std::stoi(sig), valid != "NULL", valid});
	}
	Check(sig_expected.size() == 4, "expected four lines in reference_significance_expected.tsv");

	std::ifstream sig_in("test/data/reference_significance.grib2", std::ios::binary);
	const std::vector<uint8_t> sig_file((std::istreambuf_iterator<char>(sig_in)), std::istreambuf_iterator<char>());
	Check(!sig_file.empty(), "cannot read test/data/reference_significance.grib2");
	size_t sig_pos = 0;
	size_t sig_number = 0;
	while (sig_pos + 16 <= sig_file.size() && sig_number < sig_expected.size()) {
		uint64_t length = 0;
		for (int b = 0; b < 8; b++) {
			length = (length << 8) | sig_file[sig_pos + 8 + b];
		}
		const Expected &want = sig_expected[sig_number];
		const std::string name = "significance " + std::to_string(want.significance);
		std::string error;
		Check(ScreenMessage(sig_file.data() + sig_pos, static_cast<size_t>(length), error),
		      name + ": refused by the message screen: " + error);
		codes_handle *h =
		    codes_handle_new_from_message_copy(nullptr, sig_file.data() + sig_pos, static_cast<size_t>(length));
		Check(h != nullptr, name + ": ecCodes could not open it");
		if (h) {
			const bool run_time = Grib2ReferenceTimeIsRunTime(h);
			Check(run_time == (want.significance == 0 || want.significance == 1),
			      name + ": run-time flag is " + std::to_string(run_time));
			codes_handle_delete(h);
		}
		bool prefix_run_time = false;
		const auto micros =
		    ValidTimeFromPrefix(sig_file.data() + sig_pos, static_cast<size_t>(length), prefix_run_time);
		Check(prefix_run_time == want.has_valid_time, name + ": run-time flag from the prefix is wrong");
		if (want.has_valid_time) {
			// the expectation is a date and time: compare through the same calendar function the extension uses
			const int y = std::stoi(want.valid_time.substr(0, 4)), mo = std::stoi(want.valid_time.substr(5, 2)),
			          d = std::stoi(want.valid_time.substr(8, 2)), hh = std::stoi(want.valid_time.substr(11, 2)),
			          mi = std::stoi(want.valid_time.substr(14, 2));
			const auto want_micros = Grib2MakeMicros(y, mo, d, hh, mi);
			Check(micros.has_value() && want_micros.has_value() && *micros == *want_micros,
			      name + ": valid time " + (micros ? std::to_string(*micros) : "nothing") + ", expected " +
			          want.valid_time);
		}
		sig_pos += static_cast<size_t>(length);
		sig_number++;
	}
	Check(sig_number == 4, "found " + std::to_string(sig_number) + " significance messages, expected four");

	std::cout << "checks=" << checks << " failures=" << failures << "\n";
	return failures == 0 ? 0 : 1;
}
