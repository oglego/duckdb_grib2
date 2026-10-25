#pragma once

// Minimal parser for the sections at the start of a GRIB2 message.
//
// ecCodes can create a handle from a truncated message and read most keys from it, but not the data representation
// (packing) template. Walking the section headers directly gives the packing template number, the parameter and the
// grid template from the first few hundred bytes of a message, so a "details" index can range-read a small prefix
// of each message instead of the whole thing.
//
// Only the first field of a message is described: parsing stops at the first data representation section (5).
//
// Deliberately free of DuckDB and ecCodes dependencies so it can be unit tested on its own
// (see test/cpp/idx_check.cpp).

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace duckdb {

struct MessageSummary {
	int edition = 0;
	int discipline = -1;
	//! Total length of the message in bytes, from the indicator section
	uint64_t total_length = 0;
	//! Reference time, section 1
	int year = 0;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;
	//! Grid definition template number (section 3, for example 30 is Lambert conformal) and its number of points
	int grid_template = -1;
	uint32_t grid_points = 0;
	//! Product definition template number, parameter category and number (section 4)
	int product_template = -1;
	int parameter_category = -1;
	int parameter_number = -1;
	//! Data representation template number (section 5): 0 simple, 2 complex, 3 complex with spatial differencing,
	//! 40 JPEG 2000, 42 CCSDS
	int data_template = -1;
	uint32_t data_points = 0;
};

enum class SectionParseResult {
	//! The summary is complete
	OK,
	//! The buffer ends before the information is available; retry with at least `needed` bytes
	NEED_MORE,
	//! The bytes are not a GRIB2 message this parser understands; see the error text
	INVALID
};

//! The ecCodes `packingType` name for a data representation template number, or nullptr when not known.
//! Covers the gridded templates; test/data/packing_templates.tsv checks each against ecCodes.
inline const char *DataTemplateName(int data_template) {
	switch (data_template) {
	case 0:
		return "grid_simple";
	case 2:
		return "grid_complex";
	case 3:
		return "grid_complex_spatial_differencing";
	case 4:
		return "grid_ieee";
	case 40:
		return "grid_jpeg";
	case 41:
		return "grid_png";
	case 42:
		return "grid_ccsds";
	case 61:
		return "grid_simple_log_preprocessing";
	case 200:
		return "grid_run_length";
	default:
		return nullptr;
	}
}

inline uint32_t ReadBigEndian16(const uint8_t *p) {
	return (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
}

inline uint32_t ReadBigEndian32(const uint8_t *p) {
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
	       (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint64_t ReadBigEndian64(const uint8_t *p) {
	return (static_cast<uint64_t>(ReadBigEndian32(p)) << 32) | static_cast<uint64_t>(ReadBigEndian32(p + 4));
}

//! Read the 16-byte indicator section: discipline, edition and the total message length. Needs size >= 16.
inline SectionParseResult ParseIndicator(const uint8_t *data, size_t size, MessageSummary &out, size_t &needed,
                                         std::string &error) {
	if (size < 16) {
		needed = 16;
		return SectionParseResult::NEED_MORE;
	}
	if (std::memcmp(data, "GRIB", 4) != 0) {
		error = "missing the GRIB marker";
		return SectionParseResult::INVALID;
	}
	out.discipline = data[6];
	out.edition = data[7];
	if (out.edition != 2) {
		error = "GRIB edition " + std::to_string(out.edition) + " is not supported (only edition 2)";
		return SectionParseResult::INVALID;
	}
	out.total_length = ReadBigEndian64(data + 8);
	if (out.total_length < 20) {
		error = "message length " + std::to_string(out.total_length) + " is too small";
		return SectionParseResult::INVALID;
	}
	return SectionParseResult::OK;
}

//! Describe the first field of a message from a prefix of its bytes. Returns NEED_MORE with the number of bytes
//! required when the prefix is too short; the caller may cap that at total_length once the indicator is known.
inline SectionParseResult ParseMessageSections(const uint8_t *data, size_t size, MessageSummary &out, size_t &needed,
                                               std::string &error) {
	const SectionParseResult indicator = ParseIndicator(data, size, out, needed, error);
	if (indicator != SectionParseResult::OK) {
		return indicator;
	}

	size_t pos = 16;
	while (true) {
		if (pos + 5 > out.total_length) {
			error = "the message ends before a data representation section";
			return SectionParseResult::INVALID;
		}
		if (size < pos + 5) {
			needed = pos + 5;
			return SectionParseResult::NEED_MORE;
		}
		if (std::memcmp(data + pos, "7777", 4) == 0) {
			error = "the message ends before a data representation section";
			return SectionParseResult::INVALID;
		}
		const uint32_t length = ReadBigEndian32(data + pos);
		const int number = data[pos + 4];
		if (length < 5 || pos + length > out.total_length) {
			error = "section " + std::to_string(number) + " has an invalid length " + std::to_string(length);
			return SectionParseResult::INVALID;
		}
		const bool wanted = number == 1 || number == 3 || number == 4 || number == 5;
		if (wanted && size < pos + length) {
			needed = pos + length;
			return SectionParseResult::NEED_MORE;
		}
		const uint8_t *section = data + pos;
		if (number == 1 && length >= 19) {
			out.year = static_cast<int>(ReadBigEndian16(section + 12));
			out.month = section[14];
			out.day = section[15];
			out.hour = section[16];
			out.minute = section[17];
			out.second = section[18];
		} else if (number == 3 && length >= 14 && out.grid_template < 0) {
			out.grid_points = ReadBigEndian32(section + 6);
			out.grid_template = static_cast<int>(ReadBigEndian16(section + 12));
		} else if (number == 4 && length >= 11 && out.product_template < 0) {
			out.product_template = static_cast<int>(ReadBigEndian16(section + 7));
			out.parameter_category = section[9];
			out.parameter_number = section[10];
		} else if (number == 5 && length >= 11) {
			out.data_points = ReadBigEndian32(section + 5);
			out.data_template = static_cast<int>(ReadBigEndian16(section + 9));
			return SectionParseResult::OK;
		}
		pos += length;
	}
}

} // namespace duckdb
