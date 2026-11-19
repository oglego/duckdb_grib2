// Standalone check that decoding messages from several threads at once gives the same values as decoding them one at a
// time, when every use of ecCodes is wrapped in Grib2EccodesGuard as the extension does. Needs ecCodes and the
// dependency-free headers, not DuckDB:
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -pthread -I src/include test/cpp/eccodes_threads_check.cpp -o
//   eccodes_threads_check -leccodes
//   ./eccodes_threads_check             (from the repository root)
//
// ecCodes is only safe to call from several threads when it was built that way (ECCODES_THREADS or
// ECCODES_OMP_THREADS); many distribution packages are not. The guard serialises ecCodes in that case, and this check
// is what proves it does. The scalar functions grib2_message_info and grib2_point_value reach ecCodes from several
// DuckDB threads inside one query, and separate connections reach it concurrently too.
//
//   ./eccodes_threads_check --unguarded   runs the same work without the guard, as an experiment: with an ecCodes
//                                         that is not thread-safe it may report differences or crash. Not run in CI.

#include "grib2_missing.hpp"
#include "grib2_threading.hpp"

#include <eccodes.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;

struct Checksum {
	bool ok = false;
	size_t values = 0;
	size_t missing = 0;
	double sum = 0;
	bool operator==(const Checksum &other) const {
		return ok == other.ok && values == other.values && missing == other.missing && sum == other.sum;
	}
};

static std::vector<char> ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	return std::vector<char>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

//! The ecCodes calls of DecodeGribValues: a copied handle, NaN for missing, the "values" array. The guard is held until
//! the handle is deleted, exactly as in the extension.
static Checksum Decode(const std::vector<char> &message, bool guarded) {
	Checksum result;
	std::vector<double> values;
	{
		std::unique_ptr<Grib2EccodesGuard> guard;
		if (guarded) {
			guard.reset(new Grib2EccodesGuard());
		}
		codes_handle *h = codes_handle_new_from_message_copy(nullptr, message.data(), message.size());
		if (!h) {
			return result;
		}
		size_t count = 0;
		if (Grib2UseNanForMissing(h) && codes_get_size(h, "values", &count) == CODES_SUCCESS && count > 0) {
			values.resize(count);
			if (codes_get_double_array(h, "values", values.data(), &count) == CODES_SUCCESS) {
				result.ok = true;
				values.resize(count);
			}
		}
		codes_handle_delete(h);
	}
	for (const double value : values) {
		result.values++;
		if (Grib2IsMissingValue(value)) {
			result.missing++;
		} else {
			result.sum += value;
		}
	}
	return result;
}

int main(int argc, char **argv) {
	const bool guarded = !(argc > 1 && std::strcmp(argv[1], "--unguarded") == 0);

	// Small messages covering simple, complex and spatially differenced packing, a bitmap, and three projections
	const char *paths[] = {"fixtures/nbm_gu_msg.grib2", "fixtures/nbm_pr_msg.grib2",
	                       "fixtures/nbm_hi_msg.grib2", "fixtures/hrrr_msg70_snod_spatial.grib2",
	                       "test/data/masked_gu.grib2", "test/data/simple_bitmap.grib2"};
	std::vector<std::vector<char>> messages;
	std::vector<Checksum> expected;
	for (const char *path : paths) {
		messages.push_back(ReadFile(path));
		if (messages.back().empty()) {
			std::cout << "cannot read " << path << "\n";
			return 2;
		}
		expected.push_back(Decode(messages.back(), true)); // one thread, nothing to race with
		if (!expected.back().ok) {
			std::cout << "cannot decode " << path << "\n";
			return 2;
		}
	}

	std::cout << "ecCodes " << grib_get_api_version() << ", built thread-safe: "
	          << (Grib2EccodesIsThreadSafe() ? "yes (the guard does nothing)" : "no (the guard serialises ecCodes)")
	          << ", guard " << (guarded ? "on" : "OFF (experiment)") << "\n";

	const unsigned threads = 8;
	const unsigned rounds = 40;
	std::atomic<unsigned> mismatches(0);
	std::vector<std::thread> pool;
	for (unsigned t = 0; t < threads; t++) {
		pool.emplace_back([&, t] {
			for (unsigned r = 0; r < rounds; r++) {
				const size_t which = (t + r) % messages.size();
				if (!(Decode(messages[which], guarded) == expected[which])) {
					mismatches++;
				}
			}
		});
	}
	for (auto &thread : pool) {
		thread.join();
	}
	std::cout << "decodes=" << threads * rounds << " mismatches=" << mismatches.load() << "\n";
	return mismatches.load() == 0 ? 0 : 1;
}
