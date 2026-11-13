#pragma once

// Small helpers around the ecCodes C API shared by the extension's functions.

#include "grib2_grid.hpp"
#include "grib2_message.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

// ecCodes linked through vcpkg
#include <eccodes.h>

#include <cmath>
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

inline std::string Grib2KeyAsString(codes_handle *handle, const char *key) {
	return Grib2OptionalString(handle, key).value_or("NA");
}

inline long Grib2LongKey(codes_handle *handle, const char *key, const char *function) {
	long value = 0;
	if (codes_get_long(handle, key, &value) != CODES_SUCCESS) {
		throw InvalidInputException(std::string(function) + ": GRIB key '" + key + "' is missing");
	}
	return value;
}

inline long Grib2LongKeyOrDefault(codes_handle *handle, const char *key, long default_value) {
	return static_cast<long>(Grib2OptionalLong(handle, key).value_or(default_value));
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
	// Refuse anything the screen cannot vouch for: ecCodes sizes its buffers from the message's own counts and lengths
	std::string screen_error;
	if (!ScreenMessage(reinterpret_cast<const uint8_t *>(data), static_cast<size_t>(size), screen_error)) {
		throw InvalidInputException(std::string(function) + ": " + screen_error);
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
	// Checked before the allocation: the count comes from the message
	if (value_count > static_cast<size_t>(MAX_GRID_CELLS)) {
		throw InvalidInputException(std::string(function) + ": the message has " + std::to_string(value_count) +
		                            " values; the limit is " + std::to_string(MAX_GRID_CELLS));
	}
	std::vector<double> values(value_count);
	if (value_count > 0 && codes_get_double_array(handle, "values", values.data(), &value_count) != CODES_SUCCESS) {
		throw InvalidInputException(std::string(function) + ": failed to decode values (unsupported packing?)");
	}
	values.resize(value_count);
	return values;
}

//! The decoded "values" of a message with what is needed to tell cells masked out by a bitmap from real ones.
struct DecodedValues {
	//! In ecCodes' stored order, see RawValueIndex()
	std::vector<double> values;
	bool has_bitmap = false;
	//! What ecCodes writes into masked cells (its default is 9999)
	double missing_value = 9999.0;

	bool IsMasked(double value) const {
		return has_bitmap && value == missing_value;
	}
	//! The value at a raw index, or nothing for a masked cell
	optional<double> At(int64_t raw_index) const {
		const double value = values[static_cast<size_t>(raw_index)];
		if (IsMasked(value)) {
			return optional<double>();
		}
		return value;
	}
};

//! Decode a message's values. With expected_cells >= 0 the count must match the grid, which would otherwise let a
//! corrupt message read outside the array.
inline DecodedValues DecodeValues(codes_handle *handle, const char *function, int64_t expected_cells = -1) {
	DecodedValues result;
	result.values = DecodeGribValues(handle, function);
	if (expected_cells >= 0 && static_cast<int64_t>(result.values.size()) != expected_cells) {
		throw InvalidInputException(std::string(function) + ": the message has " +
		                            std::to_string(result.values.size()) + " values but its grid has " +
		                            std::to_string(expected_cells) + " cells");
	}
	result.has_bitmap = Grib2LongKeyOrDefault(handle, "bitmapPresent", 0) != 0;
	result.missing_value = Grib2OptionalDouble(handle, "missingValue").value_or(9999.0);
	return result;
}

//! Everything that defines a grid's geometry. Two messages with equal specs have the same cells, so cell lookups can be
//! reused between them.
struct GridSpec {
	//! ecCodes' gridType: lambert, mercator or polar_stereographic
	std::string type;
	int64_t ni = 0;
	int64_t nj = 0;
	double la1 = 0;
	double lo1 = 0;
	double lad = 0;
	double dx = 0;
	double dy = 0;
	double radius = 0;
	//! Lambert: LoV. Polar stereographic: the orientation of the grid. Unused for Mercator.
	double lov = 0;
	//! Lambert only
	double latin1 = 0;
	double latin2 = 0;
	//! Rows are stored in alternating directions, see RawValueIndex()
	bool alternative_rows = false;

	bool operator==(const GridSpec &other) const {
		return type == other.type && ni == other.ni && nj == other.nj && la1 == other.la1 && lo1 == other.lo1 &&
		       lad == other.lad && dx == other.dx && dy == other.dy && radius == other.radius && lov == other.lov &&
		       latin1 == other.latin1 && latin2 == other.latin2 && alternative_rows == other.alternative_rows;
	}
};

struct Grib2Grid {
	std::unique_ptr<ProjectedGrid> grid;
	GridSpec spec;
};

//! The projection for a spec that MakeGrid has validated
inline std::unique_ptr<ProjectedGrid> BuildGrid(const GridSpec &s) {
	if (s.type == "lambert") {
		return std::make_unique<LambertGrid>(s.ni, s.nj, s.la1, s.lo1, s.lad, s.lov, s.latin1, s.latin2, s.dx, s.dy,
		                                     s.radius);
	}
	if (s.type == "polar_stereographic") {
		return std::make_unique<PolarStereographicGrid>(s.ni, s.nj, s.la1, s.lo1, s.lad, s.lov, s.dx, s.dy, s.radius);
	}
	return std::make_unique<MercatorGrid>(s.ni, s.nj, s.la1, s.lo1, s.lad, s.dx, s.dy, s.radius);
}

//! Build the grid described by a message's grid definition. Throws for grids that are not supported yet.
inline Grib2Grid MakeGrid(codes_handle *h, const char *function) {
	GridSpec spec;
	spec.type = Grib2KeyAsString(h, "gridType");
	if (spec.type != "lambert" && spec.type != "mercator" && spec.type != "polar_stereographic") {
		throw NotImplementedException(std::string(function) + ": grid type '" + spec.type +
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

	spec.ni = Grib2LongKey(h, "Ni", function);
	spec.nj = Grib2LongKey(h, "Nj", function);
	spec.la1 = Grib2DoubleKey(h, "latitudeOfFirstGridPointInDegrees", function);
	spec.lo1 = Grib2DoubleKey(h, "longitudeOfFirstGridPointInDegrees", function);
	spec.lad = Grib2DoubleKey(h, "LaDInDegrees", function);
	spec.dx = Grib2DoubleKey(h, "DxInMetres", function);
	spec.dy = Grib2DoubleKey(h, "DyInMetres", function);
	spec.radius = Grib2DoubleKey(h, "radius", function);
	spec.alternative_rows = Grib2LongKeyOrDefault(h, "alternativeRowScanning", 0) != 0;
	// A polar stereographic grid may be true at the pole itself; the cone and cylinder projections cannot
	const double max_lad = spec.type == "polar_stereographic" ? 90.0 : 89.999999;
	if (!std::isfinite(spec.lad) || std::fabs(spec.lad) > max_lad) {
		throw InvalidInputException(std::string(function) + ": the latitude where the grid is true (LaD) is not valid");
	}
	std::string grid_error;
	if (!ValidateGridParameters(spec.ni, spec.nj, spec.la1, spec.lo1, spec.dx, spec.dy, spec.radius, grid_error)) {
		throw InvalidInputException(std::string(function) + ": " + grid_error);
	}

	if (spec.type == "lambert") {
		spec.lov = Grib2DoubleKey(h, "LoVInDegrees", function);
		spec.latin1 = Grib2DoubleKey(h, "Latin1InDegrees", function);
		spec.latin2 = Grib2DoubleKey(h, "Latin2InDegrees", function);
		// Both standard parallels at the equator would make the cone constant zero (a cylinder, not a cone)
		if (!std::isfinite(spec.lov) || !std::isfinite(spec.latin1) || !std::isfinite(spec.latin2) ||
		    std::fabs(spec.latin1) >= 90.0 || std::fabs(spec.latin2) >= 90.0 ||
		    (spec.latin1 == 0.0 && spec.latin2 == 0.0)) {
			throw InvalidInputException(std::string(function) +
			                            ": the Lambert conformal standard parallels or orientation are not valid");
		}
	} else if (spec.type == "polar_stereographic") {
		if (Grib2LongKey(h, "southPoleOnProjectionPlane", function) != 0) {
			throw NotImplementedException(std::string(function) + ": south polar stereographic is not supported yet");
		}
		spec.lov = Grib2DoubleKey(h, "orientationOfTheGridInDegrees", function);
	}

	Grib2Grid result;
	result.grid = BuildGrid(spec);
	result.spec = std::move(spec);
	return result;
}

} // namespace duckdb
