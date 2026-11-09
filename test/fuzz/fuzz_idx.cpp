// libFuzzer target for the .idx parser.
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -I src/include test/fuzz/fuzz_idx.cpp -o fuzz_idx
//   ./fuzz_idx -max_total_time=60 fixtures/        (the .idx fixture seeds the corpus)
#include "grib2_idx.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	const std::string text(reinterpret_cast<const char *>(data), size);
	std::vector<duckdb::IdxEntry> entries;
	std::string error;
	duckdb::ParseIdx(text, -1, entries, error);
	duckdb::ParseIdx(text, static_cast<int64_t>(size) * 1024, entries, error);
	return 0;
}
