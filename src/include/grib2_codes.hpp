#pragma once

// Small helpers around the ecCodes C API shared by the extension's functions.

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

// ecCodes linked through vcpkg
#include <eccodes.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {

struct CodesHandleDeleter {
	void operator()(codes_handle *handle) const {
		if (handle) {
			codes_handle_delete(handle);
		}
	}
};
using CodesHandlePtr = std::unique_ptr<codes_handle, CodesHandleDeleter>;

inline std::string Grib2KeyAsString(codes_handle *handle, const char *key) {
	char buffer[256];
	size_t length = sizeof(buffer);
	if (codes_get_string(handle, key, buffer, &length) != CODES_SUCCESS) {
		return "NA";
	}
	return std::string(buffer);
}

inline long Grib2LongKey(codes_handle *handle, const char *key, const char *function) {
	long value = 0;
	if (codes_get_long(handle, key, &value) != CODES_SUCCESS) {
		throw InvalidInputException(std::string(function) + ": GRIB key '" + key + "' is missing");
	}
	return value;
}

inline long Grib2LongKeyOrDefault(codes_handle *handle, const char *key, long default_value) {
	long value = 0;
	if (codes_get_long(handle, key, &value) != CODES_SUCCESS) {
		return default_value;
	}
	return value;
}

inline double Grib2DoubleKey(codes_handle *handle, const char *key, const char *function) {
	double value = 0;
	if (codes_get_double(handle, key, &value) != CODES_SUCCESS) {
		throw InvalidInputException(std::string(function) + ": GRIB key '" + key + "' is missing");
	}
	return value;
}

//! Create an ecCodes handle for the first message in a buffer.
inline CodesHandlePtr OpenGribMessage(const char *function, const char *data, idx_t size) {
	// Cheap pre-check: a GRIB message starts with the 4-byte "GRIB" marker followed by the rest of the indicator
	// section. This also keeps ecCodes from logging errors to stderr for obviously non-GRIB input.
	if (size < 16 || std::memcmp(data, "GRIB", 4) != 0) {
		throw InvalidInputException(std::string(function) + ": input is not a valid GRIB message");
	}
	// The _copy variant makes ecCodes own its buffer, so the DuckDB vector data can be released safely.
	CodesHandlePtr handle(codes_handle_new_from_message_copy(nullptr, data, static_cast<size_t>(size)));
	if (!handle) {
		throw InvalidInputException(std::string(function) + ": input is not a valid GRIB message");
	}
	return handle;
}

//! Decode every value of the message in ecCodes' "values" order.
inline std::vector<double> DecodeGribValues(codes_handle *handle, const char *function) {
	size_t value_count = 0;
	if (codes_get_size(handle, "values", &value_count) != CODES_SUCCESS) {
		throw InvalidInputException(std::string(function) + ": could not determine the number of values");
	}
	std::vector<double> values(value_count);
	if (value_count > 0 && codes_get_double_array(handle, "values", values.data(), &value_count) != CODES_SUCCESS) {
		throw InvalidInputException(std::string(function) + ": failed to decode values (unsupported packing?)");
	}
	values.resize(value_count);
	return values;
}

//! The value of a string key, or nothing when the key is missing
inline optional<std::string> Grib2OptionalString(codes_handle *handle, const char *key) {
	char buffer[256];
	size_t length = sizeof(buffer);
	if (codes_get_string(handle, key, buffer, &length) != CODES_SUCCESS) {
		return optional<std::string>();
	}
	return std::string(buffer);
}

//! The value of an integer key, or nothing when the key is missing
inline optional<int64_t> Grib2OptionalLong(codes_handle *handle, const char *key) {
	long value = 0;
	if (codes_get_long(handle, key, &value) != CODES_SUCCESS) {
		return optional<int64_t>();
	}
	return static_cast<int64_t>(value);
}

//! The value of a numeric key, or nothing when the key is missing
inline optional<double> Grib2OptionalDouble(codes_handle *handle, const char *key) {
	double value = 0;
	if (codes_get_double(handle, key, &value) != CODES_SUCCESS) {
		return optional<double>();
	}
	return value;
}

} // namespace duckdb
