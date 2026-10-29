#pragma once

// Small helpers around the ecCodes C API shared by the extension's functions.

#include "grib2_grid.hpp"

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

struct Grib2Grid {
	std::unique_ptr<ProjectedGrid> grid;
	//! Rows are stored in alternating directions, see RawValueIndex()
	bool alternative_rows;
	//! Identifies the grid geometry: equal fingerprints mean the same cells, so lookups can be reused between messages
	std::string fingerprint;
};

//! Build the grid described by a message's grid definition. Throws for grids that are not supported yet.
inline Grib2Grid MakeGrid(codes_handle *h, const char *function) {
	const std::string grid_type = Grib2KeyAsString(h, "gridType");
	if (grid_type != "lambert" && grid_type != "mercator" && grid_type != "polar_stereographic") {
		throw NotImplementedException(std::string(function) + ": grid type '" + grid_type +
		                              "' is not supported yet (lambert, mercator and polar_stereographic are)");
	}
	// The grid code assumes a spherical earth with the radius ecCodes reports
	const long shape_of_earth = Grib2LongKey(h, "shapeOfTheEarth", function);
	if (shape_of_earth != 0 && shape_of_earth != 1 && shape_of_earth != 6 && shape_of_earth != 8) {
		throw NotImplementedException(std::string(function) + ": shape of the earth " + std::to_string(shape_of_earth) +
		                              " is not supported yet (spherical only)");
	}
	// RawValueIndex() assumes i increases east, j increases north and rows are stored consecutively
	if (Grib2LongKey(h, "iScansNegatively", function) != 0 || Grib2LongKey(h, "jScansPositively", function) != 1 ||
	    Grib2LongKey(h, "jPointsAreConsecutive", function) != 0) {
		throw NotImplementedException(std::string(function) + ": this scanning mode is not supported yet");
	}

	const int64_t ni = Grib2LongKey(h, "Ni", function);
	const int64_t nj = Grib2LongKey(h, "Nj", function);
	const double la1 = Grib2DoubleKey(h, "latitudeOfFirstGridPointInDegrees", function);
	const double lo1 = Grib2DoubleKey(h, "longitudeOfFirstGridPointInDegrees", function);
	const double lad = Grib2DoubleKey(h, "LaDInDegrees", function);
	const double dx = Grib2DoubleKey(h, "DxInMetres", function);
	const double dy = Grib2DoubleKey(h, "DyInMetres", function);
	const double radius = Grib2DoubleKey(h, "radius", function);

	Grib2Grid result;
	result.alternative_rows = Grib2LongKeyOrDefault(h, "alternativeRowScanning", 0) != 0;
	result.fingerprint = grid_type + "|" + std::to_string(ni) + "|" + std::to_string(nj) + "|" +
	                     StringUtil::Format("%.17g|%.17g|%.17g|%.17g|%.17g|%.17g", la1, lo1, lad, dx, dy, radius) +
	                     "|" + std::to_string(result.alternative_rows);
	if (grid_type == "lambert") {
		const double lov = Grib2DoubleKey(h, "LoVInDegrees", function);
		const double latin1 = Grib2DoubleKey(h, "Latin1InDegrees", function);
		const double latin2 = Grib2DoubleKey(h, "Latin2InDegrees", function);
		result.fingerprint += StringUtil::Format("|%.17g|%.17g|%.17g", lov, latin1, latin2);
		result.grid = std::make_unique<LambertGrid>(ni, nj, la1, lo1, lad, lov, latin1, latin2, dx, dy, radius);
	} else if (grid_type == "polar_stereographic") {
		if (Grib2LongKey(h, "southPoleOnProjectionPlane", function) != 0) {
			throw NotImplementedException(std::string(function) + ": south polar stereographic is not supported yet");
		}
		const double lov = Grib2DoubleKey(h, "orientationOfTheGridInDegrees", function);
		result.fingerprint += StringUtil::Format("|%.17g", lov);
		result.grid = std::make_unique<PolarStereographicGrid>(ni, nj, la1, lo1, lad, lov, dx, dy, radius);
	} else {
		result.grid = std::make_unique<MercatorGrid>(ni, nj, la1, lo1, lad, dx, dy, radius);
	}
	return result;
}

} // namespace duckdb
