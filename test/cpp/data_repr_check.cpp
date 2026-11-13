// Standalone check (no DuckDB, no ecCodes) of CheckDataRepresentation: every valid message in the test files must be
// accepted, and corrupt data representation parameters must be refused before they reach ecCodes.
//
//   g++ -std=c++17 -O2 -I src/include test/cpp/data_repr_check.cpp -o /tmp/data_repr_check && /tmp/data_repr_check
// Run from the repository root.

#include "grib2_message.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
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

static std::vector<uint8_t> ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

//! Offsets of the messages in a file
static std::vector<size_t> MessageStarts(const std::vector<uint8_t> &file) {
	std::vector<size_t> starts;
	size_t pos = 0;
	while (pos + 16 <= file.size() && std::memcmp(file.data() + pos, "GRIB", 4) == 0) {
		starts.push_back(pos);
		pos += static_cast<size_t>(ReadBigEndian64(file.data() + pos + 8));
	}
	return starts;
}

//! Screening exactly as OpenGribMessage does it; true when the message may go on to ecCodes
static bool Accepted(const std::vector<uint8_t> &msg, std::string &error) {
	MessageSummary summary;
	size_t needed = 0;
	if (ParseMessageSections(msg.data(), msg.size(), summary, needed, error) != SectionParseResult::OK) {
		return true; // left to ecCodes
	}
	return CheckMessageCounts(summary, error) && CheckDataRepresentation(msg.data(), msg.size(), summary, error);
}

static size_t FindSection(const std::vector<uint8_t> &msg, int number) {
	size_t pos = 16;
	while (pos + 5 <= msg.size()) {
		if (msg[pos + 4] == number && std::memcmp(msg.data() + pos, "7777", 4) != 0) {
			return pos;
		}
		pos += ReadBigEndian32(msg.data() + pos);
	}
	return 0;
}

static void Put32(std::vector<uint8_t> &msg, size_t at, uint32_t value) {
	for (int b = 0; b < 4; b++) {
		msg[at + b] = static_cast<uint8_t>(value >> (8 * (3 - b)));
	}
}

int main() {
	const char *files[] = {"fixtures/nbm_msg.grib2",    "fixtures/nbm_complex_msg.grib2", "fixtures/nbm_ak_msg.grib2",
	                       "fixtures/nbm_hi_msg.grib2", "fixtures/nbm_gu_msg.grib2",      "fixtures/nbm_pr_msg.grib2",
	                       "test/data/multi.grib2",     "test/data/masked_gu.grib2",      "test/data/sample.grib2",
	                       "test/data/stale.grib2"};
	int messages = 0;
	std::vector<uint8_t> complex_spatial; // template 5.3, used for the corruption cases
	std::vector<uint8_t> complex_plain;   // template 5.2
	for (const char *path : files) {
		const std::vector<uint8_t> file = ReadFile(path);
		Check(!file.empty(), std::string("cannot read ") + path);
		const std::vector<size_t> starts = MessageStarts(file);
		for (size_t index = 0; index < starts.size(); index++) {
			const size_t length = static_cast<size_t>(ReadBigEndian64(file.data() + starts[index] + 8));
			const std::vector<uint8_t> msg(file.begin() + starts[index], file.begin() + starts[index] + length);
			std::string error;
			Check(Accepted(msg, error),
			      std::string(path) + " message " + std::to_string(index + 1) + " is valid but was refused: " + error);
			messages++;
			MessageSummary summary;
			size_t needed = 0;
			std::string unused;
			if (ParseMessageSections(msg.data(), msg.size(), summary, needed, unused) == SectionParseResult::OK) {
				if (summary.data_template == 3 && complex_spatial.empty() && msg.size() < 100000) {
					complex_spatial = msg;
				}
				if (summary.data_template == 2 && complex_plain.empty()) {
					complex_plain = msg;
				}
			}
		}
	}
	Check(messages >= 12, "expected at least 12 messages in the test files, saw " + std::to_string(messages));
	Check(!complex_spatial.empty() && !complex_plain.empty(), "need a 5.2 and a 5.3 message among the test files");

	const struct {
		const char *what;
		size_t offset; // within section 5
		uint32_t value;
		int width; // bytes
	} corruptions[] = {
	    {"bits per value of 200", 19, 200, 1},
	    {"group count of 4 billion", 31, 0xFFFFFFFFu, 4},
	    {"group count above the number of values", 31, 0x7FFFFFFF, 4},
	    {"group width reference of 105", 35, 105, 1},
	    {"group width bits of 200", 36, 200, 1},
	    {"group length bits of 200", 46, 200, 1},
	};
	for (const auto &base : {complex_spatial, complex_plain}) {
		const size_t section5 = FindSection(base, 5);
		for (const auto &c : corruptions) {
			std::vector<uint8_t> msg = base;
			if (c.width == 4) {
				Put32(msg, section5 + c.offset, c.value);
			} else {
				msg[section5 + c.offset] = static_cast<uint8_t>(c.value);
			}
			std::string error;
			Check(!Accepted(msg, error), std::string("corruption not refused: ") + c.what);
			Check(!error.empty(), std::string("no error text for: ") + c.what);
		}
	}
	{ // spatial differencing parameters (5.3 only)
		const size_t section5 = FindSection(complex_spatial, 5);
		for (const uint8_t order : {uint8_t(3), uint8_t(255)}) {
			std::vector<uint8_t> msg = complex_spatial;
			msg[section5 + 47] = order;
			std::string error;
			Check(!Accepted(msg, error), "spatial differencing order " + std::to_string(order) + " not refused");
		}
		std::vector<uint8_t> msg = complex_spatial;
		msg[section5 + 48] = 200;
		std::string error;
		Check(!Accepted(msg, error), "200 octets per extra descriptor not refused");
	}
	{ // a data section too short for the group tables it claims
		std::vector<uint8_t> msg = complex_spatial;
		const size_t section5 = FindSection(complex_spatial, 5);
		const size_t section7 = FindSection(msg, 7);
		Put32(msg, section7, 5 + 8); // section 7 with an 8 byte payload
		msg.resize(section7 + 5 + 8 + 4);
		std::memcpy(msg.data() + msg.size() - 4, "7777", 4);
		for (int b = 0; b < 8; b++) {
			msg[8 + b] = static_cast<uint8_t>(static_cast<uint64_t>(msg.size()) >> (8 * (7 - b)));
		}
		(void)section5;
		std::string error;
		Check(!Accepted(msg, error), "a section 7 too short for the group tables was not refused");
	}
	{ // layout: the cases that made ecCodes abort or crash when they reached it
		const std::vector<uint8_t> masked = ReadFile("test/data/masked_gu.grib2");
		Check(!masked.empty(), "cannot read test/data/masked_gu.grib2");
		const size_t section6 = FindSection(masked, 6);
		std::string error;
		Check(Accepted(masked, error), "the masked fixture is valid: " + error);
		{ // the bitmap section length cut from 4663 to 55 bytes: the next 'section' starts inside the bitmap
			std::vector<uint8_t> msg = masked;
			Put32(msg, section6, 55);
			Check(!Accepted(msg, error), "a section 6 shortened below its bitmap was not refused");
		}
		{ // a bitmap with fewer bits than grid points (section 6 shrunk consistently, rest of the message moved up)
			std::vector<uint8_t> msg = masked;
			const size_t old_length = ReadBigEndian32(&msg[section6]);
			Put32(msg, section6, 7); // one bitmap byte
			msg.erase(msg.begin() + section6 + 7, msg.begin() + section6 + old_length);
			for (int b = 0; b < 8; b++) {
				msg[8 + b] = static_cast<uint8_t>(static_cast<uint64_t>(msg.size()) >> (8 * (7 - b)));
			}
			Check(!Accepted(msg, error), "a bitmap shorter than the grid was not refused");
		}
		{ // no closing 7777
			std::vector<uint8_t> msg = masked;
			msg[msg.size() - 1] = '8';
			Check(!Accepted(msg, error), "a message without the 7777 end section was not refused");
		}
		{ // the indicator claims more bytes than were supplied
			std::vector<uint8_t> msg = masked;
			msg.resize(msg.size() - 10);
			Check(!Accepted(msg, error), "a message shorter than its declared length was not refused");
		}
		{ // a section number that does not exist
			std::vector<uint8_t> msg = masked;
			msg[section6 + 4] = 9;
			Check(!Accepted(msg, error), "an unknown section number was not refused");
		}
	}
	std::cout << "checks=" << checks << " messages=" << messages << " failures=" << failures << "\n";
	return failures ? 1 : 0;
}
