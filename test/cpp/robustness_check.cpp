// Robustness checks for the dependency-free parsers and grid validation.
//
// Feeds the .idx parser, the GRIB2 section parser and the grid validator truncated, bit-flipped and random input and
// requires only that they return (true or false) without crashing or reading out of bounds. Meant to be built with
// -fsanitize=address,undefined so a memory error fails the run:
//
//   g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -I src/include
//   test/cpp/robustness_check.cpp
//       -o robustness_check && ./robustness_check
//
// The input is deterministic (fixed seed), so a failure reproduces.

#include "grib2_grid.hpp"
#include "grib2_idx.hpp"
#include "grib2_message.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace duckdb;

static int failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

static std::string ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void ParseSections(const std::string &bytes) {
	MessageSummary summary;
	size_t needed = 0;
	std::string error;
	ParseMessageSections(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), summary, needed, error);
	MessageSummary indicator;
	ParseIndicator(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), indicator, needed, error);
}

static void ParseIdxText(const std::string &text, int64_t file_size) {
	std::vector<IdxEntry> entries;
	std::string error;
	ParseIdx(text, file_size, entries, error);
}

int main() {
	std::mt19937_64 rng(12345);

	// --- Section parser: every truncation and many mutations of each real message ----------------------------------
	const std::vector<std::string> messages = {
	    "fixtures/nbm_msg.grib2",    "fixtures/nbm_complex_msg.grib2", "fixtures/nbm_gu_msg.grib2",
	    "fixtures/nbm_hi_msg.grib2", "fixtures/nbm_pr_msg.grib2",      "fixtures/nbm_ak_msg.grib2",
	    "test/data/sample.grib2",    "test/data/masked_gu.grib2",
	};
	size_t inputs = 0;
	for (const auto &path : messages) {
		const std::string original = ReadFile(path);
		CHECK(!original.empty());
		if (original.empty()) {
			continue;
		}
		// The headers are in the first few hundred bytes; truncate at every length there, then a sample beyond
		const size_t head = std::min<size_t>(original.size(), 1024);
		for (size_t len = 0; len <= head; len++) {
			ParseSections(original.substr(0, len));
			inputs++;
		}
		for (int round = 0; round < 4000; round++) {
			std::string mutated = original.substr(0, head);
			const int flips = 1 + static_cast<int>(rng() % 6);
			for (int f = 0; f < flips; f++) {
				mutated[rng() % mutated.size()] = static_cast<char>(rng() & 0xff);
			}
			// Cut at a random point too, so the parser meets a header whose declared length runs past the buffer
			if (rng() % 3 == 0) {
				mutated.resize(rng() % (mutated.size() + 1));
			}
			ParseSections(mutated);
			inputs++;
		}
	}
	// Random bytes, and random bytes behind a valid-looking indicator section with an enormous declared length
	for (int round = 0; round < 20000; round++) {
		std::string bytes(rng() % 256, '\0');
		for (auto &c : bytes) {
			c = static_cast<char>(rng() & 0xff);
		}
		if (bytes.size() >= 16 && (rng() & 1)) {
			std::memcpy(&bytes[0], "GRIB", 4);
			bytes[7] = 2;
			for (int b = 8; b < 16; b++) {
				bytes[b] = static_cast<char>(rng() & 0xff);
			}
		}
		ParseSections(bytes);
		inputs++;
	}

	// --- .idx parser -----------------------------------------------------------------------------------------------
	const std::string idx = ReadFile("fixtures/blend.t00z.core.f001.co.grib2.idx");
	CHECK(!idx.empty());
	for (size_t len = 0; len <= std::min<size_t>(idx.size(), 4096); len++) {
		ParseIdxText(idx.substr(0, len), -1);
		ParseIdxText(idx.substr(0, len), 1LL << 40);
		inputs++;
	}
	for (int round = 0; round < 20000; round++) {
		std::string mutated = idx.substr(0, std::min<size_t>(idx.size(), 2048));
		if (mutated.empty()) {
			break;
		}
		const int flips = 1 + static_cast<int>(rng() % 8);
		for (int f = 0; f < flips; f++) {
			mutated[rng() % mutated.size()] = static_cast<char>(rng() & 0xff);
		}
		ParseIdxText(mutated, (rng() & 1) ? -1 : static_cast<int64_t>(rng() % (1LL << 40)));
		inputs++;
	}
	// Hand-written awkward lines
	const char *awkward[] = {"",
	                         ":",
	                         "::::::",
	                         "1:0:d=2026102300:TMP:2 m above ground:1 hour fcst",
	                         "1:0:d=2026102300:TMP:2 m above ground:1 hour fcst:prob >",
	                         "1:0:d=2026102300:TMP:2 m above ground:1 hour fcst:prob >=1 <",
	                         "1:0:d=2026102300:TMP:2 m above ground: hour fcst:",
	                         "1:0:d=2026102300:TMP:2 m above ground:99999999999999999999 hour fcst:",
	                         "9223372036854775807:9223372036854775807:d=2026102300:A:B:C:",
	                         "1:0:d=2026023100:A:B:C:",
	                         "1:0:d=:A:B:C:",
	                         "1.2:0:d=2026102300:A:B:C:"};
	for (const char *line : awkward) {
		ParseIdxText(line, -1);
		ParseIdxText(std::string(line) + "\n2:10:d=2026102300:A:B:C:\n", 100);
	}
	// Offsets near int64 max must not overflow the length arithmetic
	ParseIdxText("1:9223372036854775800:d=2026102300:A:B:C:\n2:9223372036854775806:d=2026102300:A:B:C:\n",
	             std::numeric_limits<int64_t>::max());

	// --- Grid validation -------------------------------------------------------------------------------------------
	std::string error;
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	CHECK(ValidateGridParameters(2345, 1597, 19.2, 233.7, 2539.7, 2539.7, 6371200.0, error));
	CHECK(!ValidateGridParameters(0, 10, 0, 0, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(10, 0, 0, 0, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(-1, 10, 0, 0, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(1LL << 40, 1LL << 40, 0, 0, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(std::numeric_limits<int64_t>::max(), 2, 0, 0, 1000, 1000, 6371200, error));
	CHECK(ValidateGridParameters(MAX_GRID_CELLS, 1, 0, 0, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(MAX_GRID_CELLS + 1, 1, 0, 0, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(10, 10, 91, 0, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(10, 10, nan, 0, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(10, 10, 0, inf, 1000, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(10, 10, 0, 0, 0, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(10, 10, 0, 0, 1000, -5, 6371200, error));
	CHECK(!ValidateGridParameters(10, 10, 0, 0, nan, 1000, 6371200, error));
	CHECK(!ValidateGridParameters(10, 10, 0, 0, 1000, 1000, 0, error));
	CHECK(!ValidateGridParameters(10, 10, 0, 0, 1000, 1000, inf, error));

	// A grid that passes validation must give finite answers for points on and off the grid
	LambertGrid lambert(2345, 1597, 19.229, 233.723, 25.0, 265.0, 25.0, 25.0, 2539.703, 2539.703, 6371200.0);
	const double probes[][2] = {{0, 0}, {90, 0}, {-90, 0}, {37.2, -93.3}, {37.2, 266.7}, {89.9999999, 12}, {45, 1e9}};
	for (const auto &probe : probes) {
		GridCell cell;
		if (lambert.NearestCell(probe[0], probe[1], cell)) {
			CHECK(cell.i >= 0 && cell.i < 2345 && cell.j >= 0 && cell.j < 1597);
			CHECK(std::isfinite(cell.distance_km));
		}
		std::vector<int64_t> cells;
		lambert.CellsInBox(probe[1] - 1, probe[0] - 1, probe[1] + 1, probe[0] + 1, cells);
		for (int64_t c : cells) {
			CHECK(c >= 0 && c < 2345LL * 1597LL);
		}
	}

	std::printf("robustness: %zu parser inputs, failures=%d\n", inputs, failures);
	return failures == 0 ? 0 : 1;
}
