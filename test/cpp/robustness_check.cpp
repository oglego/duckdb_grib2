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
	    "fixtures/nbm_msg.grib2",
	    "fixtures/nbm_complex_msg.grib2",
	    "fixtures/nbm_gu_msg.grib2",
	    "fixtures/nbm_hi_msg.grib2",
	    "fixtures/nbm_pr_msg.grib2",
	    "fixtures/nbm_ak_msg.grib2",
	    "test/data/sample.grib2",
	    "test/data/masked_gu.grib2",
	    "fixtures/hrrr_msg90_csnow_simple_const.grib2",
	    "fixtures/hrrr_msg166_icec_spatial.grib2",
	    "fixtures/hrrr_msg70_snod_spatial.grib2",
	    "test/data/simple_window.grib2",
	    "test/data/simple_bitmap.grib2",
	    "test/data/simple_9999.grib2",
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

	// --- Regressions: hostile lengths, steps and counts -------------------------------------------------------------
	// These reproduce bugs found in review. Each one failed before its fix (an endless walk that grew memory without
	// bound, a signed overflow, an allocation sized by the message), so they stay here to keep it from coming back.
	{
		const std::string a = ReadFile("fixtures/nbm_ak_msg.grib2");
		CHECK(a.size() > 100);

		const auto with_length = [](std::string message, uint64_t length) {
			for (int k = 0; k < 8; k++) {
				message[8 + k] = static_cast<char>((length >> (56 - 8 * k)) & 0xff);
			}
			return message;
		};
		const auto indicator_result = [](const std::string &message) {
			MessageSummary summary;
			size_t needed = 0;
			std::string err;
			return ParseIndicator(reinterpret_cast<const uint8_t *>(message.data()), message.size(), summary, needed,
			                      err);
		};
		// Walk a file the way IndexFile() does without an .idx. Returns the number of messages found, or -1 when the
		// walk stopped with an error. Gives up after `cap` steps so a walk that never ends is reported, not run.
		const auto walk = [](const std::string &file, int cap) {
			const int64_t size = static_cast<int64_t>(file.size());
			int64_t offset = 0;
			int count = 0;
			while (offset < size) {
				if (count >= cap) {
					return -2; // did not end
				}
				const std::string prefix = file.substr(static_cast<size_t>(offset), 512);
				MessageSummary summary;
				size_t needed = 0;
				std::string err;
				if (ParseMessageSections(reinterpret_cast<const uint8_t *>(prefix.data()), prefix.size(), summary,
				                         needed, err) != SectionParseResult::OK) {
					return -1;
				}
				int64_t length = 0;
				if (!MessageLengthWithinFile(summary.total_length, offset, size, length, err)) {
					return -1;
				}
				if (offset + length > size || length < 20) {
					return -3; // the helper let an impossible length through
				}
				count++;
				offset += length;
			}
			return count;
		};

		// A normal two-message file walks to the end
		CHECK(walk(a + a, 10) == 2);

		// Lengths of 2^63 and above used to turn negative; 2^62 + 1 and above are not plausible either
		const uint64_t hostile[] = {1ULL << 63, (1ULL << 63) + 1, (1ULL << 62) + 1,
		                            ~0ULL,      ~0ULL - 1,        0 - static_cast<uint64_t>(a.size())};
		for (uint64_t length : hostile) {
			CHECK(indicator_result(with_length(a, length)) == SectionParseResult::INVALID);
			// The original bug: a second message whose length sends the walk back to the start
			CHECK(walk(a + with_length(a, length), 10) == -1);
			CHECK(walk(with_length(a, length), 10) == -1);
		}
		CHECK(walk(a + with_length(a, a.size() + 1), 10) == -1); // runs past the end of the file
		CHECK(walk(a + with_length(a, 19), 10) == -1);           // too short to be a message

		// Any 64-bit length in either message: the walk must end, with an error or at the end of the file
		for (int round = 0; round < 20000; round++) {
			const uint64_t length = rng();
			const std::string file = (rng() & 1) ? a + with_length(a, length) : with_length(a, length) + a;
			CHECK(walk(file, 10) != -2 && walk(file, 10) != -3);
		}

		// Helper edge cases
		int64_t length = 0;
		std::string err;
		CHECK(MessageLengthWithinFile(100, 0, 100, length, err) && length == 100);
		CHECK(!MessageLengthWithinFile(101, 0, 100, length, err));
		CHECK(!MessageLengthWithinFile(100, 1, 100, length, err));
		CHECK(!MessageLengthWithinFile(19, 0, 100, length, err));
		CHECK(!MessageLengthWithinFile(100, -1, 100, length, err));
		CHECK(!MessageLengthWithinFile(100, 101, 100, length, err));
		CHECK(!MessageLengthWithinFile(~0ULL, 0, std::numeric_limits<int64_t>::max(), length, err));

		// Forecast steps from an .idx: bounded, so converting to microseconds cannot overflow
		const auto step_of = [](const std::string &forecast) {
			IdxEntry entry;
			std::string error;
			const bool ok = ParseIdxLine("1:0:d=2026102300:TMP:2 m above ground:" + forecast + ":", entry, error);
			return ok ? entry.step_hours : std::optional<int64_t>();
		};
		CHECK(step_of("anl") == std::optional<int64_t>(0));
		CHECK(step_of("0 hour fcst") == std::optional<int64_t>(0));
		CHECK(step_of("18 hour fcst") == std::optional<int64_t>(18));
		CHECK(step_of("1000000 hour fcst") == std::optional<int64_t>(IDX_MAX_STEP_HOURS));
		CHECK(!step_of("1000001 hour fcst").has_value());
		CHECK(!step_of("9223372036854775807 hour fcst").has_value());
		CHECK(!step_of("-1 hour fcst").has_value());
		CHECK(!step_of("-9223372036854775808 hour fcst").has_value());
		CHECK(!step_of("15 min fcst").has_value());
		CHECK(!step_of("0-1 hour acc fcst").has_value());
		// The arithmetic read_grib2 performs for valid_time, at the largest step and the latest reference time
		const int64_t latest_reference_us = 253402300799000000LL; // 9999-12-31 23:59:59 UTC
		int64_t microseconds = 0;
		CHECK(!__builtin_mul_overflow(IDX_MAX_STEP_HOURS, 3600000000LL, &microseconds));
		int64_t valid_us = 0;
		CHECK(!__builtin_add_overflow(latest_reference_us, microseconds, &valid_us));

		// Point counts: stop corrupt or hostile counts before ecCodes sees them
		MessageSummary counts;
		counts.grid_points = 3700000;
		counts.data_points = 3700000;
		CHECK(CheckMessageCounts(counts, err));
		counts.grid_points = static_cast<uint32_t>(MAX_GRID_CELLS);
		counts.data_points = static_cast<uint32_t>(MAX_GRID_CELLS);
		CHECK(CheckMessageCounts(counts, err));
		counts.grid_points = static_cast<uint32_t>(MAX_GRID_CELLS) + 1;
		CHECK(!CheckMessageCounts(counts, err));
		counts.grid_points = 100;
		counts.data_points = 0xFFFFFFFFu;
		CHECK(!CheckMessageCounts(counts, err));
		counts.grid_points = 0xFFFFFFFFu;
		counts.data_points = 100;
		CHECK(!CheckMessageCounts(counts, err));
	}

	{ // The reader refuses to fetch headers over the limit, judging from the first bytes alone
		const std::string huge = ReadFile("test/data/huge_header.grib2");
		CHECK(huge.size() > static_cast<size_t>(MAX_HEADER_BYTES));
		MessageSummary summary;
		size_t needed = 0;
		std::string err;
		const SectionParseResult result =
		    ParseMessageSections(reinterpret_cast<const uint8_t *>(huge.data()), 512, summary, needed, err);
		CHECK(result == SectionParseResult::NEED_MORE);
		CHECK(needed > static_cast<size_t>(MAX_HEADER_BYTES));
		CHECK(!HeaderSizeAcceptable(needed, err));
		CHECK(err.find("limit is") != std::string::npos);
		// Real headers are far below the limit, and the limit itself is accepted
		CHECK(HeaderSizeAcceptable(static_cast<size_t>(MAX_HEADER_BYTES), err));
		CHECK(!HeaderSizeAcceptable(static_cast<size_t>(MAX_HEADER_BYTES) + 1, err));
		for (const char *path : {"fixtures/nbm_msg.grib2", "fixtures/nbm_ak_msg.grib2", "fixtures/nbm_gu_msg.grib2",
		                         "fixtures/hrrr_msg70_snod_spatial.grib2", "test/data/simple_9999.grib2"}) {
			const std::string real = ReadFile(path);
			MessageSummary real_summary;
			size_t real_needed = 0;
			const SectionParseResult real_result = ParseMessageSections(reinterpret_cast<const uint8_t *>(real.data()),
			                                                            real.size(), real_summary, real_needed, err);
			CHECK(real_result == SectionParseResult::OK);
			CHECK(real_needed < 4096);
			CHECK(HeaderSizeAcceptable(real_needed, err));
		}
	}

	{ // A bbox longitude written with any multiple of 360 added selects the same cells
		MercatorGrid guam(193, 193, 12.349883999999999, 143.68653800000001, 20, 2500, 2500, 6371200);
		struct Box {
			double lon_min, span, lat_min, lat_max;
		};
		// lon_min in [-180, 180) and an eastward span in (0, 360), so the canonical box is [lon_min, lon_min + span]
		const Box boxes[] = {
		    {-60, 270, 0, 30}, {143, 3, 12, 15}, {170, 20, 0, 30}, {140, 50, 0, 30}, {-170, 40, 5, 25}};
		for (const Box &box : boxes) {
			std::vector<int64_t> expected;
			guam.CellsInBox(box.lon_min, box.lat_min, box.lon_min + box.span, box.lat_max, expected);
			for (const int shift_min : {-1, 0, 1}) {
				for (const int shift_max : {-1, 0, 1}) {
					const double lon_min = box.lon_min + 360.0 * shift_min;
					const double lon_max = box.lon_min + box.span + 360.0 * shift_max;
					const bool accepted_range = std::fabs(lon_min) <= 360.0 && std::fabs(lon_max) <= 360.0;
					if (!accepted_range || lon_max - lon_min >= 360.0) {
						continue; // outside what read_grib2 accepts, or a full turn or more (covers every longitude)
					}
					std::vector<int64_t> cells;
					guam.CellsInBox(lon_min, box.lat_min, lon_max, box.lat_max, cells);
					CHECK(cells == expected);
				}
			}
		}
		// The case that selected nothing: [300, -150] is [-60, 210], which covers the whole Guam grid
		std::vector<int64_t> wrapped, canonical;
		guam.CellsInBox(300, 0, -150, 30, wrapped);
		guam.CellsInBox(-60, 0, 210, 30, canonical);
		CHECK(wrapped.size() == 193 * 193);
		CHECK(wrapped == canonical);
		// A box 360 degrees or more wide still covers every longitude
		for (const double lon_min : {-180.0, 0.0, -360.0}) {
			std::vector<int64_t> world;
			guam.CellsInBox(lon_min, -90, lon_min + (lon_min == -360.0 ? 720.0 : 360.0), 90, world);
			CHECK(world.size() == 193 * 193);
		}
		// A narrow box across the antimeridian that misses Guam, and a zero-width box, still select nothing
		std::vector<int64_t> across, meridian;
		guam.CellsInBox(170, 0, -170, 30, across);
		CHECK(across.empty());
		guam.CellsInBox(0, 0, 0, 30, meridian);
		CHECK(meridian.empty());
	}

	std::printf("robustness: %zu parser inputs, failures=%d\n", inputs, failures);
	return failures == 0 ? 0 : 1;
}
