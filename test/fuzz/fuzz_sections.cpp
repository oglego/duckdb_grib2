// libFuzzer target for the GRIB2 section-header parser.
//   clang++ -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -I src/include test/fuzz/fuzz_sections.cpp
//       -o fuzz_sections
//   ./fuzz_sections -max_total_time=60 test/data/ (the .grib2 fixtures seed the corpus)
#include "grib2_message.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	duckdb::MessageSummary summary;
	size_t needed = 0;
	std::string error;
	duckdb::ParseMessageSections(data, size, summary, needed, error);
	return 0;
}
