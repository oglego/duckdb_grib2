// Standalone check that cells with no data are recognised however the message marks them. Needs ecCodes and the
// dependency-free headers, not DuckDB:
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -I src/include test/cpp/missing_value_check.cpp -o missing_value_check
//   -leccodes
//   ./missing_value_check          (from the repository root)
//
// It runs the same calls as DecodeGribValues (the screen, a copied handle, Grib2UseNanForMissing, the "values" array)
// and compares the cells it reports as missing with the block that the generators in test/data/ leave without data
// (generate_masked_sample.py, generate_missing_value_samples.py, generate_simple_packed_samples.py). It also replays
// the legacy rule (ecCodes' default 9999 marker, honoured only when the message has a bitmap) to prove that these
// fixtures distinguish the two: if the legacy rule ever agrees with the truth on them, they have stopped testing what
// they are for.

#include "grib2_message.hpp"
#include "grib2_missing.hpp"

#include <eccodes.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
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

struct Decoded {
	bool ok = false;
	std::vector<double> values;
	long ni = 0, nj = 0, alt = 0, bitmap = 0;
};

//! Decode as read_grib2 does. With nan_marker false, decode the legacy way: ecCodes' default marker.
static Decoded Decode(const std::vector<uint8_t> &msg, bool nan_marker) {
	Decoded d;
	std::string error;
	if (!ScreenMessage(msg.data(), msg.size(), error)) {
		std::cout << "  screen refused: " << error << "\n";
		return d;
	}
	codes_handle *h = codes_handle_new_from_message_copy(nullptr, msg.data(), msg.size());
	if (!h) {
		return d;
	}
	codes_get_long(h, "Ni", &d.ni);
	codes_get_long(h, "Nj", &d.nj);
	codes_get_long(h, "alternativeRowScanning", &d.alt);
	codes_get_long(h, "bitmapPresent", &d.bitmap);
	if (nan_marker && !Grib2UseNanForMissing(h)) {
		codes_handle_delete(h);
		return d;
	}
	size_t count = 0;
	codes_get_size(h, "values", &count);
	d.values.resize(count);
	d.ok = codes_get_double_array(h, "values", d.values.data(), &count) == CODES_SUCCESS;
	codes_handle_delete(h);
	return d;
}

//! A block of cells left without data: i in [i0, i1), j in [j0, j1), i east and j north from the first point.
//! Empty when i1 <= i0.
struct Block {
	long i0, i1, j0, j1;
};

static const Block NO_BLOCK = {0, 0, 0, 0};

//! Stored-order indexes of the cells of a block
static std::set<size_t> BlockCells(const Decoded &d, const Block &block) {
	std::set<size_t> cells;
	for (long j = block.j0; j < block.j1; j++) {
		for (long i = block.i0; i < block.i1; i++) {
			cells.insert(static_cast<size_t>(j * d.ni + ((d.alt && j % 2) ? (d.ni - 1 - i) : i)));
		}
	}
	return cells;
}

struct Case {
	const char *path;
	Block block;             // the cells that have no data
	long real_9999;          // how many other cells are exactly 9999.0; -1 when the file has none to count
	bool legacy_must_differ; // the fixture exists to tell the legacy rule from the new one
};

int main() {
	// The Guam fixtures leave i 60..79, j 100..119 empty; the 5.0 windows have their own blocks (see the generators)
	const Block guam = {60, 80, 100, 120};
	const Case cases[] = {
	    {"test/data/missing_complex.grib2", guam, -1, true},    // 5.2, flagged by the packing, no bitmap
	    {"test/data/missing_spatial.grib2", guam, -1, true},    // 5.3, flagged by the packing, no bitmap
	    {"test/data/real_9999.grib2", guam, 37249 - 400, true}, // bitmap; every other cell is a real 9999
	    {"test/data/masked_gu.grib2", guam, -1, false},         // bitmap; the original masked fixture
	    {"fixtures/nbm_gu_msg.grib2", NO_BLOCK, -1, false},     // nothing missing at all
	    {"fixtures/nbm_msg.grib2", NO_BLOCK, -1, false},
	    {"fixtures/hrrr_msg90_csnow_simple_const.grib2", NO_BLOCK, -1, false}, // real HRRR 5.0 (constant field)
	    {"fixtures/hrrr_msg166_icec_spatial.grib2", NO_BLOCK, -1, false},      // real HRRR 5.3, 2 bits per value
	    {"fixtures/hrrr_msg70_snod_spatial.grib2", NO_BLOCK, -1, false},       // real HRRR 5.3, 17 bits per value
	    {"test/data/simple_window.grib2", NO_BLOCK, -1, false},                // 5.0, no bitmap, real values
	    {"test/data/simple_bitmap.grib2", {60, 80, 30, 45}, -1, false},        // 5.0, bitmap masks a block
	    {"test/data/simple_9999.grib2", {20, 40, 50, 65}, 1064, true},         // 5.0, bitmap; 1064 real 9999s
	};
	for (const Case &c : cases) {
		std::cout << c.path << "\n";
		const std::vector<uint8_t> msg = ReadFile(c.path);
		Check(!msg.empty(), std::string("cannot read ") + c.path);
		const Decoded d = Decode(msg, true);
		Check(d.ok, std::string("decode failed for ") + c.path);
		if (!d.ok) {
			continue;
		}
		std::set<size_t> missing;
		size_t equal_9999 = 0;
		for (size_t k = 0; k < d.values.size(); k++) {
			if (Grib2IsMissingValue(d.values[k])) {
				missing.insert(k);
			} else if (d.values[k] == 9999.0) {
				equal_9999++;
			}
		}
		const std::set<size_t> expected = BlockCells(d, c.block);
		Check(missing == expected, std::string(c.path) + ": cells reported missing: " + std::to_string(missing.size()) +
		                               ", expected " + std::to_string(expected.size()));
		if (c.real_9999 >= 0) {
			Check(equal_9999 == static_cast<size_t>(c.real_9999),
			      std::string(c.path) + ": real 9999 values: " + std::to_string(equal_9999) + ", expected " +
			          std::to_string(c.real_9999));
		}

		// The legacy rule: default marker, honoured only when the message has a bitmap
		const Decoded legacy = Decode(msg, false);
		Check(legacy.ok, std::string("legacy decode failed for ") + c.path);
		size_t legacy_missing = 0;
		for (const double v : legacy.values) {
			if (legacy.bitmap != 0 && v == 9999.0) {
				legacy_missing++;
			}
		}
		const bool legacy_agrees = legacy_missing == expected.size();
		if (c.legacy_must_differ) {
			Check(!legacy_agrees, std::string(c.path) + ": the legacy rule gets this right (" +
			                          std::to_string(legacy_missing) + " cells), so the fixture no longer tests it");
		} else {
			Check(legacy_agrees, std::string(c.path) + ": the legacy rule and the new one disagree on a plain message");
		}
		std::cout << "  cells=" << d.values.size() << " missing=" << missing.size() << " real9999=" << equal_9999
		          << " legacy_rule_missing=" << legacy_missing << "\n";
	}
	std::cout << "checks=" << checks << " failures=" << failures << "\n";
	return failures == 0 ? 0 : 1;
}
