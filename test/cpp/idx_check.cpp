// Standalone check of src/include/grib2_idx.hpp and src/include/grib2_message.hpp.
//
// Build and run from the repository root:
//   g++ -std=c++17 -O2 -I src/include test/cpp/idx_check.cpp -o /tmp/idx_check && /tmp/idx_check
//
// Reference data comes from test/data/generate_message_reference.py (an independent regex implementation of the
// .idx format, and ecCodes for the message sections).

#include "grib2_idx.hpp"
#include "grib2_message.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace duckdb;

static int failures = 0;
static int checks = 0;

static void Check(bool ok, const std::string &what) {
	checks++;
	if (!ok) {
		failures++;
		if (failures <= 25) {
			std::cerr << "FAIL: " << what << "\n";
		}
	}
}

static std::string ReadFile(const std::string &path) {
	std::ifstream in(path, std::ios::binary);
	if (!in) {
		std::cerr << "cannot open " << path << " (run from the repository root)\n";
		std::exit(2);
	}
	std::ostringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

static std::vector<std::string> SplitTabs(const std::string &line) {
	std::vector<std::string> fields;
	size_t start = 0;
	while (true) {
		const size_t tab = line.find('\t', start);
		if (tab == std::string::npos) {
			fields.push_back(line.substr(start));
			return fields;
		}
		fields.push_back(line.substr(start, tab - start));
		start = tab + 1;
	}
}

static std::vector<std::vector<std::string>> ReadTsv(const std::string &path) {
	std::vector<std::vector<std::string>> rows;
	std::istringstream in(ReadFile(path));
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line[0] != '#') {
			rows.push_back(SplitTabs(line));
		}
	}
	return rows;
}

static bool SameOptionalDouble(const std::optional<double> &actual, const std::string &expected) {
	if (expected == "NA") {
		return !actual.has_value();
	}
	return actual.has_value() && std::fabs(*actual - std::atof(expected.c_str())) < 1e-9;
}

static std::string TwoDigits(int value) {
	char buffer[8];
	std::snprintf(buffer, sizeof(buffer), "%02d", value);
	return buffer;
}

// ---- Individual lines, including the awkward forms found in real NBM files
static void CheckLines() {
	IdxEntry e;
	std::string error;

	Check(ParseIdxLine("133:123034124:d=2026102300:TMP:2 m above ground:1 hour fcst:", e, error), "plain line");
	Check(e.message_number == 133 && e.byte_offset == 123034124 && e.variable == "TMP" &&
	          e.level == "2 m above ground" && e.forecast == "1 hour fcst" && e.qualifier.empty() &&
	          e.step_hours == 1 && !e.percentile && !e.threshold_lower && !e.threshold_upper,
	      "plain line fields");
	Check(e.year == 2026 && e.month == 10 && e.day == 23 && e.hour == 0 && e.minute == 0, "reference time");
	Check(e.Inventory() == "TMP:2 m above ground:1 hour fcst", "inventory without qualifier");

	Check(ParseIdxLine("134:125133436:d=2026102300:TMP:2 m above ground:1 hour fcst:ens std dev", e, error),
	      "line without trailing colon");
	Check(e.qualifier == "ens std dev" && e.Inventory() == "TMP:2 m above ground:1 hour fcst:ens std dev",
	      "qualifier without trailing colon");

	Check(ParseIdxLine("5:100:d=2026102300:TMP:2 m above ground:1 hour fcst:50% level", e, error), "percentile");
	Check(e.percentile && *e.percentile == 50.0, "percentile value");

	Check(ParseIdxLine("6:200:d=2026102300:VIS:surface:1 hour fcst:prob <304.8:prob fcst 3/7:probability forecast", e,
	                   error),
	      "probability with colons in the qualifier");
	Check(e.qualifier == "prob <304.8:prob fcst 3/7:probability forecast" && !e.threshold_lower && e.threshold_upper &&
	          *e.threshold_upper == 304.8,
	      "upper threshold");

	Check(ParseIdxLine("7:300:d=2026102300:APCP:surface:0-1 hour acc fcst:prob >=1 <2:prob fcst 1/1", e, error),
	      "probability interval");
	Check(e.threshold_lower && *e.threshold_lower == 1.0 && e.threshold_upper && *e.threshold_upper == 2.0 &&
	          !e.step_hours,
	      "interval bounds, accumulation has no step");

	Check(ParseIdxLine("8:400:d=2026102300:APCP:surface:0-1 hour acc fcst:prob >0.00254:prob fcst 255/255", e, error),
	      "probability with a decimal threshold");
	Check(e.threshold_lower && std::fabs(*e.threshold_lower - 0.00254) < 1e-12 && !e.threshold_upper, "lower only");

	Check(ParseIdxLine("9:500:d=2026102300:TMP:2 m above ground:anl", e, error), "forecast with no trailing colon");
	Check(e.forecast == "anl" && e.step_hours == 0 && e.qualifier.empty(), "analysis step is zero");

	Check(ParseIdxLine("10:600:d=2026102300:var discipline=0 center=7 local_table=1 parmcat=0 parm=206:reserved:1 hour "
	                   "fcst:\r",
	                   e, error),
	      "unnamed variable and CRLF");
	Check(e.variable == "var discipline=0 center=7 local_table=1 parmcat=0 parm=206" && e.level == "reserved" &&
	          e.forecast == "1 hour fcst",
	      "unnamed variable fields");

	Check(ParseIdxLine("11:700:d=202610230630:TMP:2 m above ground:anl:", e, error) && e.hour == 6 && e.minute == 30,
	      "reference time with minutes");

	// Malformed input is rejected with a message, not parsed into nonsense
	Check(!ParseIdxLine("1:0:2026102300:TMP:2 m above ground:1 hour fcst:", e, error) &&
	          error.find("reference time") != std::string::npos,
	      "missing d=");
	Check(!ParseIdxLine("1:abc:d=2026102300:TMP:surface:anl:", e, error) &&
	          error.find("byte offset") != std::string::npos,
	      "non-numeric offset");
	Check(!ParseIdxLine("1.2:0:d=2026102300:TMP:surface:anl:", e, error) &&
	          error.find("not supported") != std::string::npos,
	      "n.m message number");
	Check(!ParseIdxLine("1:0:d=2026102300:TMP", e, error), "too few fields");
	Check(!ParseIdxLine("1:0:d=2026132300:TMP:surface:anl:", e, error), "invalid month");

	std::vector<IdxEntry> entries;
	Check(!ParseIdx("1:100:d=2026102300:A:l:anl:\n2:100:d=2026102300:B:l:anl:\n", -1, entries, error) &&
	          error.find("line 2") != std::string::npos,
	      "offsets must increase");
	Check(ParseIdx("1:0:d=2026102300:A:l:anl:\r\n\r\n2:50:d=2026102300:B:l:anl:\r\n", 120, entries, error) &&
	          entries.size() == 2 && entries[0].byte_length == 50 && entries[1].byte_length == 70,
	      "CRLF and blank lines, last length from file size");
	Check(ParseIdx("1:0:d=2026102300:A:l:anl:\n2:50:d=2026102300:B:l:anl:\n", -1, entries, error) &&
	          entries[1].byte_length == -1,
	      "last length unknown without a file size");
	Check(!ParseIdx("1:0:d=2026102300:A:l:anl:\n2:50:d=2026102300:B:l:anl:\n", 50, entries, error),
	      "file size must exceed the last offset");
}

// ---- The real 154-message NBM inventory against the independent expected parse
static void CheckNbmIdx() {
	std::vector<IdxEntry> entries;
	std::string error;
	const int64_t file_size = 156464859;
	Check(ParseIdx(ReadFile("fixtures/blend.t00z.core.f001.co.grib2.idx"), file_size, entries, error),
	      "parse NBM idx: " + error);
	const auto expected = ReadTsv("test/data/nbm_idx_expected.tsv");
	Check(entries.size() == expected.size() && entries.size() == 154, "NBM message count");
	for (size_t i = 0; i < entries.size() && i < expected.size(); i++) {
		const IdxEntry &e = entries[i];
		const auto &x = expected[i];
		const std::string who = "NBM message " + std::to_string(i + 1) + ": ";
		const std::string time = std::to_string(e.year) + "-" + TwoDigits(e.month) + "-" + TwoDigits(e.day) + "T" +
		                         TwoDigits(e.hour) + ":" + TwoDigits(e.minute) + ":00";
		Check(std::to_string(e.message_number) == x[0], who + "message number");
		Check(std::to_string(e.byte_offset) == x[1], who + "offset");
		Check(std::to_string(e.byte_length) == x[2], who + "length");
		Check(time == x[3], who + "reference time " + time + " vs " + x[3]);
		Check(e.variable == x[4], who + "variable");
		Check(e.level == x[5], who + "level");
		Check(e.forecast == x[6], who + "forecast");
		Check(e.qualifier == x[7], who + "qualifier '" + e.qualifier + "' vs '" + x[7] + "'");
		Check(x[8] == "NA" ? !e.step_hours.has_value()
		                   : (e.step_hours.has_value() && std::to_string(*e.step_hours) == x[8]),
		      who + "step hours");
		Check(SameOptionalDouble(e.percentile, x[9]), who + "percentile");
		Check(SameOptionalDouble(e.threshold_lower, x[10]), who + "lower threshold");
		Check(SameOptionalDouble(e.threshold_upper, x[11]), who + "upper threshold");
	}

	// Facts established while designing the index, from the real file
	int plain = 0, percentile = 0, probability = 0, std_dev = 0, bounded = 0, reserved = 0;
	for (const IdxEntry &e : entries) {
		plain += e.qualifier.empty();
		percentile += e.percentile.has_value();
		probability += e.qualifier.compare(0, 4, "prob") == 0;
		std_dev += e.qualifier == "ens std dev";
		bounded += e.threshold_lower.has_value() || e.threshold_upper.has_value();
		reserved += e.level == "reserved";
	}
	Check(plain == 52 && percentile == 58 && probability == 37 && std_dev == 6, "qualifier category counts");
	Check(bounded == 36, "36 of the 37 probability qualifiers carry a threshold (one is just 'probability forecast')");
	Check(reserved == 9, "levels named 'reserved'");
	int repeated = 0;
	for (size_t i = 0; i < entries.size(); i++) {
		for (size_t j = i + 1; j < entries.size(); j++) {
			repeated += entries[i].Inventory() == entries[j].Inventory();
		}
	}
	Check(repeated > 0, "inventory text is not unique: message number is the only key");
}

// Walk a file message by message using only the 16-byte indicator sections
static std::vector<std::pair<int64_t, int64_t>> HopMessages(const std::string &data) {
	std::vector<std::pair<int64_t, int64_t>> spans;
	size_t pos = 0;
	while (pos < data.size()) {
		MessageSummary summary;
		size_t needed = 0;
		std::string error;
		const auto *bytes = reinterpret_cast<const uint8_t *>(data.data()) + pos;
		if (ParseIndicator(bytes, data.size() - pos, summary, needed, error) != SectionParseResult::OK) {
			Check(false, "hop at byte " + std::to_string(pos) + ": " + error);
			break;
		}
		spans.emplace_back(static_cast<int64_t>(pos), static_cast<int64_t>(summary.total_length));
		pos += summary.total_length;
	}
	return spans;
}

// ---- The multi-message fixture: sidecar offsets must agree with the message boundaries found by hopping
static void CheckMulti() {
	const std::string data = ReadFile("test/data/multi.grib2");
	std::vector<IdxEntry> entries;
	std::string error;
	Check(ParseIdx(ReadFile("test/data/multi.grib2.idx"), static_cast<int64_t>(data.size()), entries, error),
	      "parse multi idx: " + error);
	const auto spans = HopMessages(data);
	Check(spans.size() == 4 && entries.size() == 4, "multi fixture has four messages");
	for (size_t i = 0; i < spans.size() && i < entries.size(); i++) {
		Check(entries[i].byte_offset == spans[i].first && entries[i].byte_length == spans[i].second,
		      "multi message " + std::to_string(i + 1) + " offset and length agree with the indicator sections");
	}
	Check(entries.size() == 4 && entries[0].variable == "CAPE" && entries[3].forecast == "anl", "multi idx content");
}

// ---- Section headers against ecCodes, including parsing from short prefixes
static void CheckSections() {
	for (const auto &row : ReadTsv("test/data/sections_reference.tsv")) {
		const std::string &path = row[0];
		const std::string data = ReadFile(path);
		const auto *bytes = reinterpret_cast<const uint8_t *>(data.data());

		// Grow the prefix until the parser is satisfied, the way a ranged reader would
		size_t available = 16;
		MessageSummary summary;
		SectionParseResult result = SectionParseResult::NEED_MORE;
		std::string error;
		int rounds = 0;
		while (result == SectionParseResult::NEED_MORE && rounds++ < 20) {
			summary = MessageSummary();
			size_t needed = 0;
			result = ParseMessageSections(bytes, std::min(available, data.size()), summary, needed, error);
			if (result == SectionParseResult::NEED_MORE) {
				Check(needed > available, path + ": needed bytes must grow");
				available = needed;
			}
		}
		Check(result == SectionParseResult::OK, path + ": sections parse: " + error);
		Check(available < 1000, path + ": the header prefix is small (" + std::to_string(available) + " bytes)");

		Check(summary.edition == 2, path + ": edition");
		Check(std::to_string(summary.discipline) == row[1], path + ": discipline");
		Check(std::to_string(summary.total_length) == row[2] && summary.total_length == data.size(),
		      path + ": total length");
		Check(std::to_string(summary.year) == row[3] && std::to_string(summary.month) == row[4] &&
		          std::to_string(summary.day) == row[5] && std::to_string(summary.hour) == row[6] &&
		          std::to_string(summary.minute) == row[7],
		      path + ": reference time");
		Check(std::to_string(summary.grid_template) == row[8], path + ": grid template");
		Check(std::to_string(summary.grid_points) == row[9], path + ": grid points");
		Check(std::to_string(summary.product_template) == row[10], path + ": product template");
		Check(std::to_string(summary.parameter_category) == row[11], path + ": parameter category");
		Check(std::to_string(summary.parameter_number) == row[12], path + ": parameter number");
		Check(std::to_string(summary.data_template) == row[13], path + ": data representation template");
		Check(std::to_string(summary.data_points) == row[14], path + ": data points");
	}

	// Input that is not a GRIB2 message
	MessageSummary summary;
	size_t needed = 0;
	std::string error;
	const uint8_t not_grib[32] = {'N', 'O', 'P', 'E'};
	Check(ParseMessageSections(not_grib, sizeof(not_grib), summary, needed, error) == SectionParseResult::INVALID,
	      "not a GRIB message");
	const uint8_t tiny[4] = {'G', 'R', 'I', 'B'};
	Check(ParseMessageSections(tiny, sizeof(tiny), summary, needed, error) == SectionParseResult::NEED_MORE &&
	          needed == 16,
	      "too short for an indicator section");
	uint8_t edition1[16] = {'G', 'R', 'I', 'B', 0, 0, 0, 1};
	Check(ParseMessageSections(edition1, sizeof(edition1), summary, needed, error) == SectionParseResult::INVALID &&
	          error.find("edition") != std::string::npos,
	      "GRIB edition 1 is rejected");
}

int main() {
	CheckLines();
	CheckNbmIdx();
	CheckMulti();
	CheckSections();
	std::printf("checks=%d failures=%d\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
