// Mutation fuzzer for the ecCodes decode path. Standalone: needs ecCodes and the dependency-free headers, not DuckDB.
//
//   g++ -std=c++17 -O2 -I src/include test/fuzz/eccodes_mutate.cpp -o eccodes_mutate -leccodes
//   ./eccodes_mutate fixtures/nbm_gu_msg.grib2 [iterations=2000] [seed=1] [crash_dir=/tmp/grib2_crashes]
//   ./eccodes_mutate --replay /tmp/grib2_crashes/some_input.grib2     (prints the ecCodes call in progress)
//
// Each mutated message is decoded in a forked child, so a crash, abort, exit() or hang in ecCodes is recorded instead
// of ending the run. The child repeats what read_grib2 does before and after ecCodes: the indicator and length check,
// the section parser and count limits (inputs refused there never reach ecCodes), then
// codes_handle_new_from_message_copy, the grid keys MakeGrid reads, and the full "values" decode.
//
// Exit status of the fuzzer is 1 when any input made ecCodes crash, exit or hang after passing the extension's checks.

#include "grib2_message.hpp"

#include <eccodes.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

using duckdb::MAX_GRID_CELLS;
using duckdb::MessageSummary;
using duckdb::ParseIndicator;
using duckdb::ParseMessageSections;
using duckdb::ReadBigEndian32;
using duckdb::SectionParseResult;

namespace {

// Child exit codes with a meaning of their own; anything else is an ecCodes exit() and counts as a failure
enum ChildResult {
	DECODED = 0,
	SCREENED = 10, // refused by the extension's own checks before ecCodes
	NO_HANDLE = 11,
	NO_SIZE = 12,
	TOO_MANY_VALUES = 13,
	DECODE_ERROR = 14
};

struct Section {
	size_t start;
	size_t length;
	int number;
};

std::vector<Section> FindSections(const std::vector<uint8_t> &msg) {
	std::vector<Section> sections;
	size_t pos = 16;
	while (pos + 5 <= msg.size()) {
		if (msg[pos] == '7' && msg[pos + 1] == '7' && msg[pos + 2] == '7' && msg[pos + 3] == '7') {
			break;
		}
		const size_t length = ReadBigEndian32(&msg[pos]);
		if (length < 5 || pos + length > msg.size()) {
			break;
		}
		sections.push_back({pos, length, msg[pos + 4]});
		pos += length;
	}
	return sections;
}

void PutBigEndian64(std::vector<uint8_t> &msg, size_t at, uint64_t value) {
	for (int b = 0; b < 8; b++) {
		msg[at + b] = static_cast<uint8_t>(value >> (8 * (7 - b)));
	}
}

void PutBigEndian32(std::vector<uint8_t> &msg, size_t at, uint32_t value) {
	for (int b = 0; b < 4; b++) {
		msg[at + b] = static_cast<uint8_t>(value >> (8 * (3 - b)));
	}
}

const char *Mutate(std::vector<uint8_t> &msg, const std::vector<Section> &sections, std::mt19937_64 &rng) {
	auto pick = [&](size_t n) {
		return static_cast<size_t>(rng() % n);
	};
	const uint32_t extremes[] = {0, 1, 2, 255, 65535, 65536, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu};
	// Sections whose contents drive grid geometry and decoding: 3 grid, 5 data representation, 6 bitmap, 7 data
	std::vector<Section> targets;
	for (const auto &s : sections) {
		if (s.number == 3 || s.number == 5 || s.number == 6 || s.number == 7) {
			targets.push_back(s);
		}
	}
	if (targets.empty()) {
		targets = sections;
	}
	switch (pick(6)) {
	case 0: { // random bytes anywhere
		const size_t count = 1 + pick(4);
		for (size_t k = 0; k < count; k++) {
			msg[pick(msg.size())] ^= static_cast<uint8_t>(1 + pick(255));
		}
		return "flip-anywhere";
	}
	case 1: { // random bytes in a section header (the parameters), where the template fields live
		const Section &s = targets[pick(targets.size())];
		const size_t span = std::min<size_t>(s.length, 96);
		const size_t count = 1 + pick(4);
		for (size_t k = 0; k < count; k++) {
			msg[s.start + pick(span)] ^= static_cast<uint8_t>(1 + pick(255));
		}
		return "flip-section-header";
	}
	case 2: { // a 32-bit field replaced by an extreme value
		const Section &s = targets[pick(targets.size())];
		const size_t span = std::min<size_t>(s.length, 96);
		if (span >= 4) {
			PutBigEndian32(msg, s.start + pick(span - 3), extremes[pick(sizeof(extremes) / sizeof(extremes[0]))]);
		}
		return "extreme-field";
	}
	case 3: { // a run of zero or 0xFF bytes in the data section
		const Section &s = sections.empty() ? Section {16, msg.size() - 16, 7} : sections.back();
		const size_t at = s.start + pick(s.length);
		const size_t run = 1 + pick(64);
		const uint8_t fill = pick(2) ? 0x00 : 0xFF;
		for (size_t k = at; k < std::min(msg.size(), at + run); k++) {
			msg[k] = fill;
		}
		return "fill-run";
	}
	case 4: { // truncation, with the indicator length corrected so it still gets past the length check
		if (msg.size() > 40) {
			msg.resize(20 + pick(msg.size() - 20));
			PutBigEndian64(msg, 8, msg.size());
		}
		return "truncate";
	}
	default: { // a section length field that disagrees with the real layout
		const Section &s = sections[pick(sections.size())];
		PutBigEndian32(msg, s.start, extremes[pick(sizeof(extremes) / sizeof(extremes[0]))]);
		return "bad-section-length";
	}
	}
}

bool g_trace = false;
bool g_skip_data_check = false; // GRIB2_FUZZ_NO_DATA_CHECK=1 reproduces the behaviour before CheckDataRepresentation
void Stage(const char *name, const char *detail = "") {
	if (g_trace) {
		fprintf(stderr, "stage: %s %s\n", name, detail);
	}
}

[[noreturn]] void RunChild(const std::vector<uint8_t> &msg) {
	rlimit limit {};
	limit.rlim_cur = limit.rlim_max = 3ULL << 30; // memory blow-ups show up as failures, not as a stuck machine
	setrlimit(RLIMIT_AS, &limit);
	alarm(30);
	if (!g_trace) {
		const int null_fd = open("/dev/null", O_WRONLY);
		if (null_fd >= 0) {
			dup2(null_fd, STDERR_FILENO);
		}
	}

	// What LoadMessage and OpenGribMessage do before ecCodes sees the bytes
	MessageSummary indicator;
	size_t needed = 0;
	std::string error;
	if (ParseIndicator(msg.data(), msg.size(), indicator, needed, error) != SectionParseResult::OK ||
	    indicator.total_length != msg.size()) {
		_exit(SCREENED);
	}
	MessageSummary summary;
	if (ParseMessageSections(msg.data(), msg.size(), summary, needed, error) == SectionParseResult::OK &&
	    (!duckdb::CheckMessageCounts(summary, error) ||
	     (!g_skip_data_check && !duckdb::CheckDataRepresentation(msg.data(), msg.size(), summary, error)))) {
		_exit(SCREENED);
	}

	Stage("codes_handle_new_from_message_copy");
	codes_handle *h = codes_handle_new_from_message_copy(nullptr, msg.data(), msg.size());
	if (!h) {
		_exit(NO_HANDLE);
	}
	char text[256];
	size_t text_len = sizeof(text);
	Stage("key", "gridType");
	codes_get_string(h, "gridType", text, &text_len);
	long l = 0;
	double d = 0;
	for (const char *key : {"shapeOfTheEarth", "iScansNegatively", "jScansPositively", "jPointsAreConsecutive",
	                        "alternativeRowScanning", "Ni", "Nj", "bitmapPresent", "numberOfPoints"}) {
		Stage("key", key);
		codes_get_long(h, key, &l);
	}
	for (const char *key :
	     {"radius", "LoVInDegrees", "LaDInDegrees", "Latin1InDegrees", "Latin2InDegrees", "DxInMetres", "DyInMetres",
	      "latitudeOfFirstGridPointInDegrees", "longitudeOfFirstGridPointInDegrees", "missingValue"}) {
		Stage("key", key);
		codes_get_double(h, key, &d);
	}
	text_len = sizeof(text);
	Stage("key", "shortName");
	codes_get_string(h, "shortName", text, &text_len);

	size_t count = 0;
	Stage("codes_get_size", "values");
	if (codes_get_size(h, "values", &count) != CODES_SUCCESS) {
		_exit(NO_SIZE);
	}
	if (count > static_cast<size_t>(MAX_GRID_CELLS)) {
		_exit(TOO_MANY_VALUES);
	}
	std::vector<double> values(count);
	Stage("codes_get_double_array", "values");
	if (count > 0 && codes_get_double_array(h, "values", values.data(), &count) != CODES_SUCCESS) {
		_exit(DECODE_ERROR);
	}
	double sum = 0;
	for (size_t k = 0; k < count; k++) {
		sum += values[k];
	}
	codes_handle_delete(h);
	_exit(sum == sum + 1 ? DECODED : DECODED); // keeps the loop from being optimised away
}

std::string Classify(int status, bool &failure) {
	failure = false;
	if (WIFSIGNALED(status)) {
		failure = true;
		const int sig = WTERMSIG(status);
		if (sig == SIGALRM) {
			return "timeout (30 s)";
		}
		return std::string("signal ") + strsignal(sig);
	}
	switch (WEXITSTATUS(status)) {
	case DECODED:
		return "decoded";
	case SCREENED:
		return "refused by extension checks";
	case NO_HANDLE:
		return "ecCodes: no handle";
	case NO_SIZE:
		return "ecCodes: no size";
	case TOO_MANY_VALUES:
		return "over MAX_GRID_CELLS";
	case DECODE_ERROR:
		return "ecCodes: decode error";
	default:
		failure = true;
		return "ecCodes called exit(" + std::to_string(WEXITSTATUS(status)) + ")";
	}
}

} // namespace

int main(int argc, char **argv) {
	g_skip_data_check = std::getenv("GRIB2_FUZZ_NO_DATA_CHECK") != nullptr;
	if (argc == 3 && std::string(argv[1]) == "--replay") { // trace one saved input: --replay crash.grib2
		g_trace = true;
		std::ifstream replay(argv[2], std::ios::binary);
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(replay)), std::istreambuf_iterator<char>());
		RunChild(bytes);
	}
	if (argc < 2) {
		std::cerr << "usage: eccodes_mutate file.grib2 [iterations] [seed] [crash_dir]\n";
		return 2;
	}
	std::ifstream in(argv[1], std::ios::binary);
	const std::vector<uint8_t> original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	const int iterations = argc > 2 ? std::atoi(argv[2]) : 2000;
	const uint64_t seed = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
	const std::string crash_dir = argc > 4 ? argv[4] : "/tmp/grib2_crashes";
	mkdir(crash_dir.c_str(), 0755);

	const std::vector<Section> sections = FindSections(original);
	if (original.size() < 40 || sections.empty()) {
		std::cerr << "not a GRIB2 message: " << argv[1] << "\n";
		return 2;
	}
	std::mt19937_64 rng(seed);
	std::map<std::string, int> outcomes;
	std::map<std::string, int> failures_by_mutation;
	int failures = 0;

	for (int iteration = 0; iteration < iterations; iteration++) {
		std::vector<uint8_t> msg = original;
		const char *mutation = Mutate(msg, sections, rng);
		fflush(nullptr);
		const pid_t pid = fork();
		if (pid == 0) {
			RunChild(msg);
		}
		int status = 0;
		waitpid(pid, &status, 0);
		bool failure = false;
		const std::string outcome = Classify(status, failure);
		outcomes[outcome]++;
		if (failure) {
			failures++;
			failures_by_mutation[mutation]++;
			const std::string path = crash_dir + "/" +
			                         std::string(argv[1]).substr(std::string(argv[1]).rfind('/') + 1) + "." +
			                         std::to_string(seed) + "." + std::to_string(iteration) + ".grib2";
			std::ofstream out(path, std::ios::binary);
			out.write(reinterpret_cast<const char *>(msg.data()), static_cast<std::streamsize>(msg.size()));
			if (failures <= 5) {
				std::cout << "  FAIL " << outcome << " after " << mutation << " -> " << path << "\n";
			}
		}
	}
	std::cout << argv[1] << ": " << iterations << " inputs, seed " << seed << "\n";
	for (const auto &o : outcomes) {
		std::cout << "  " << o.second << "\t" << o.first << "\n";
	}
	if (failures) {
		std::cout << "  failures by mutation:";
		for (const auto &m : failures_by_mutation) {
			std::cout << " " << m.first << "=" << m.second;
		}
		std::cout << "\n";
	}
	return failures ? 1 : 0;
}
