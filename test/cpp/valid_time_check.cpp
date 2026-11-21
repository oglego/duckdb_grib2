// Checks Grib2ValidTimeMicros, the valid time read from the message, against valid times computed independently.
//
//  * the calendar arithmetic, against values computed with Python's calendar.timegm, including leap days, the 1900
//    non-leap century, the earliest and latest years a TIMESTAMP holds, and dates that do not exist
//  * the eight messages in test/data/valid_times.grib2 (minutes, days and 3-hour units, interval templates, a leap day,
//    a year boundary), against test/data/valid_times_expected.tsv
//  * that the extension's own message screen accepts every one of those messages
//
// Build and run from the repository root (see .github/workflows/GridCheck.yml). Needs ecCodes, not DuckDB.

#include "grib2_message.hpp"
#include "grib2_valid_time.hpp"

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
			codes_handle_delete(h);
		}
		pos += static_cast<size_t>(length);
		number++;
	}
	Check(number == 8, "found " + std::to_string(number) + " messages, expected eight");

	std::cout << "checks=" << checks << " failures=" << failures << "\n";
	return failures == 0 ? 0 : 1;
}
