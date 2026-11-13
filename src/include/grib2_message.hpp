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

#include "grib2_grid.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace duckdb {

//! Longest message length accepted from an indicator section. The field is 64 bits, and a hostile or corrupt value of
//! 2^63 or more turns negative when stored in an int64_t, which makes "offset + length" wrap and lets a file send the
//! reader backwards forever. 2^62 is far beyond any real message and keeps all offset arithmetic in range.
constexpr uint64_t MAX_MESSAGE_LENGTH_FIELD = 1ULL << 62;

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
	if (out.total_length > MAX_MESSAGE_LENGTH_FIELD) {
		error = "message length " + std::to_string(out.total_length) + " is not plausible";
		return SectionParseResult::INVALID;
	}
	return SectionParseResult::OK;
}

//! The length of the message at `offset` in a file of `file_size` bytes, for walking a file with no .idx. The message
//! must fit in the rest of the file, and since it is at least 20 bytes long (checked by the indicator parser) every
//! step moves forward, so the walk always ends. Returns false and sets `error` when the message does not fit.
inline bool MessageLengthWithinFile(uint64_t total_length, int64_t offset, int64_t file_size, int64_t &length,
                                    std::string &error) {
	if (offset < 0 || offset > file_size || total_length < 20 ||
	    total_length > static_cast<uint64_t>(file_size - offset)) {
		error = "the file ends inside this message (truncated?)";
		return false;
	}
	length = static_cast<int64_t>(total_length);
	return true;
}

//! Refuse messages that claim more grid or data points than the extension will decode. ecCodes sizes its buffers from
//! these counts, so a corrupt or hostile count must be stopped before the message reaches it.
inline bool CheckMessageCounts(const MessageSummary &summary, std::string &error) {
	if (static_cast<int64_t>(summary.grid_points) > MAX_GRID_CELLS ||
	    static_cast<int64_t>(summary.data_points) > MAX_GRID_CELLS) {
		error = "the message claims " + std::to_string(summary.grid_points) + " grid points and " +
		        std::to_string(summary.data_points) + " data points; the limit is " + std::to_string(MAX_GRID_CELLS);
		return false;
	}
	return true;
}

//! Unsigned integer of `bits` (0 to 32) bits starting at bit `position` of a big-endian bit stream. The caller has
//! checked that the bits lie inside the buffer.
inline uint64_t ReadBitsBigEndian(const uint8_t *data, uint64_t position, unsigned bits) {
	if (bits == 0) {
		return 0;
	}
	const uint8_t *first = data + (position >> 3);
	const unsigned shift = static_cast<unsigned>(position & 7);
	const unsigned bytes = (shift + bits + 7) / 8; // at most 5, so the window fits in 64 bits
	uint64_t window = 0;
	for (unsigned k = 0; k < bytes; k++) {
		window = (window << 8) | first[k];
	}
	window >>= bytes * 8 - shift - bits;
	return window & ((uint64_t(1) << bits) - 1);
}

//! Reject malformed GRIB2 messages before ecCodes sees them. ecCodes trusts the input: bad lengths, absurd group
//! counts, or bit widths above 64 can abort or segfault the decoder. We do a basic whole-message check: sections must
//! be contiguous up to the final "7777" marker, and the field must satisfy a few sanity checks that no valid message
//! can violate. Other packing templates are left to ecCodes.
inline bool CheckDataRepresentation(const uint8_t *data, size_t size, const MessageSummary &summary,
                                    std::string &error) {
	if (summary.total_length > size) {
		error = "the message is shorter than the length in its indicator section";
		return false;
	}
	const uint64_t total = summary.total_length;
	if (total < 20 || std::memcmp(data + total - 4, "7777", 4) != 0) {
		error = "the message does not end with the 7777 end section";
		return false;
	}
	const uint8_t *section5 = nullptr;
	uint64_t section5_length = 0;
	const uint8_t *section6 = nullptr;
	uint64_t section6_length = 0;
	const uint8_t *section7 = nullptr;
	uint64_t section7_length = 0;
	uint64_t pos = 16;
	while (pos < total - 4) {
		const uint64_t length = pos + 5 <= total - 4 ? ReadBigEndian32(data + pos) : 0;
		const int number = pos + 5 <= total - 4 ? data[pos + 4] : 0;
		if (length < 5 || pos + length > total - 4 || number < 1 || number > 7) {
			error = "the sections of the message do not add up to its length";
			return false;
		}
		if (number == 5 && !section5) {
			section5 = data + pos;
			section5_length = length;
		} else if (number == 6 && section5 && !section6) {
			section6 = data + pos;
			section6_length = length;
		} else if (number == 7 && section6 && !section7) {
			section7 = data + pos;
			section7_length = length;
		}
		pos += length;
	}
	if (!section5 || !section6 || !section7) {
		error = "the message has no complete data representation, bitmap and data sections";
		return false;
	}
	// An explicit bitmap (indicator 0) carries one bit per grid point
	if (section6_length >= 6 && section6[5] == 0 && (section6_length - 6) * 8 < summary.grid_points) {
		error = "the bitmap is too short for the number of grid points";
		return false;
	}
	if (summary.data_template != 0 && summary.data_template != 2 && summary.data_template != 3) {
		return true;
	}
	const bool spatial = summary.data_template == 3;
	const uint64_t required_length = summary.data_template == 0 ? 20 : (spatial ? 49 : 47);
	if (section5_length < required_length) {
		error = "the data representation section is shorter than its template requires";
		return false;
	}
	const uint64_t values = ReadBigEndian32(section5 + 5);
	const uint64_t bits_per_value = section5[19];
	const uint64_t payload_bits = (section7_length - 5) * 8;
	if (bits_per_value > 64) {
		error = "the data representation section declares " + std::to_string(bits_per_value) + " bits per value";
		return false;
	}
	if (summary.data_template == 0) {
		if (values * bits_per_value > payload_bits) {
			error = "the data section is too short for the declared number of packed values";
			return false;
		}
		return true;
	}
	// Complex packing: octets 32-35 number of groups, 36 reference for group widths, 37 bits for group widths, 47 bits
	// for scaled group lengths; with spatial differencing octets 48 (order) and 49 (octets per extra descriptor)
	const uint64_t groups = ReadBigEndian32(section5 + 31);
	const uint64_t width_reference = section5[35];
	const uint64_t width_bits = section5[36];
	const uint64_t length_bits = section5[46];
	if (width_reference > 64 || width_bits > 32 || length_bits > 32) {
		error = "the data representation section declares group widths or lengths that cannot be stored";
		return false;
	}
	if (groups > values) {
		error = "the data representation section declares " + std::to_string(groups) + " groups for " +
		        std::to_string(values) + " values";
		return false;
	}
	uint64_t descriptor_bytes = 0;
	if (spatial) {
		const uint64_t order = section5[47];
		const uint64_t octets = section5[48];
		if (order > 2 || octets > 8) {
			error = "the data representation section declares an unsupported spatial differencing";
			return false;
		}
		descriptor_bytes = (order + 1) * octets;
	}
	// Section 7 holds the extra descriptors, then three bit-packed tables of one entry per group (references, widths,
	// scaled lengths; each padded to a whole octet), then the packed values of every group one after the other
	const uint64_t payload_bytes = section7_length - 5;
	const uint64_t reference_bytes = (groups * bits_per_value + 7) / 8;
	const uint64_t widths_bytes = (groups * width_bits + 7) / 8;
	const uint64_t lengths_bytes = (groups * length_bits + 7) / 8;
	const uint64_t tables_bytes = descriptor_bytes + reference_bytes + widths_bytes + lengths_bytes;
	if (tables_bytes > payload_bytes) {
		error = "the data section is too short for the declared group tables";
		return false;
	}
	// Walk the tables the way the decoder will: the groups must not hold more values than the message declares, and
	// their packed values must fit in what is left of the data section. Anything else is read past the buffer.
	const uint8_t *widths = section7 + 5 + descriptor_bytes + reference_bytes;
	const uint8_t *lengths = widths + widths_bytes;
	const uint64_t length_reference = ReadBigEndian32(section5 + 37);
	const uint64_t length_increment = section5[41];
	const uint64_t last_length = ReadBigEndian32(section5 + 42);
	const uint64_t packed_bits = (payload_bytes - tables_bytes) * 8;
	uint64_t total_values = 0;
	uint64_t total_bits = 0;
	for (uint64_t group = 0; group < groups; group++) {
		const uint64_t width =
		    width_reference + ReadBitsBigEndian(widths, group * width_bits, static_cast<unsigned>(width_bits));
		uint64_t length = last_length;
		if (group + 1 < groups) {
			length =
			    length_reference +
			    ReadBitsBigEndian(lengths, group * length_bits, static_cast<unsigned>(length_bits)) * length_increment;
		}
		if (width > 64) {
			error = "the data representation section declares a group wider than 64 bits";
			return false;
		}
		total_values += length;
		if (total_values > values) {
			error = "the groups hold more values than the " + std::to_string(values) + " the message declares";
			return false;
		}
		total_bits += length * width; // at most values * 64, which cannot overflow
		if (total_bits > packed_bits) {
			error = "the data section is too short for the packed group values";
			return false;
		}
	}
	return true;
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
