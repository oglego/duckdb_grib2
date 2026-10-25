#define DUCKDB_EXTENSION_MAIN

#include "grib2_extension.hpp"
#include "grib2_grid.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// ecCodes linked through vcpkg
#include <eccodes.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace duckdb {

// Smoke-test function: reports the version of the linked ecCodes library.
// Confirms that ecCodes is built, linked and callable from inside the extension.
// ecCodes encodes its version as major * 10000 + minor * 100 + revision.
inline void Grib2EccodesVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	const long version = grib_get_api_version();
	const auto version_str =
	    StringUtil::Format("%d.%d.%d", static_cast<int>(version / 10000), static_cast<int>((version / 100) % 100),
	                       static_cast<int>(version % 100));
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<string_t>(result)[0] = StringVector::AddString(result, version_str);
}

// ---------------------------------------------------------------------------
// Temporary scaffolding while grib2_index() and read_grib2() are built:
//
//   grib2_message_info(BLOB) -> VARCHAR
//     Decodes the first GRIB message in a buffer and describes it. Validates that ecCodes definitions and the
//     message's packing template work from inside the extension.
//
//   grib2_point_value(BLOB, lat, lon) -> DOUBLE
//     Value at the grid cell nearest to a point. Exercises the full path used by read_grib2(): grid keys, scanning
//     mode checks, nearest-cell lookup, value ordering and decoding. NULL when the point is outside the grid.
//     Supports the projections NBM uses: Lambert conformal, polar stereographic and Mercator.
//     Decodes the whole message once per row, so it is only meant for tests and spot checks.
// ---------------------------------------------------------------------------
struct CodesHandleDeleter {
	void operator()(codes_handle *handle) const {
		if (handle) {
			codes_handle_delete(handle);
		}
	}
};
using CodesHandlePtr = std::unique_ptr<codes_handle, CodesHandleDeleter>;

static std::string Grib2KeyAsString(codes_handle *handle, const char *key) {
	char buffer[256];
	size_t length = sizeof(buffer);
	if (codes_get_string(handle, key, buffer, &length) != CODES_SUCCESS) {
		return "NA";
	}
	return std::string(buffer);
}

static long Grib2LongKey(codes_handle *handle, const char *key, const char *function) {
	long value = 0;
	if (codes_get_long(handle, key, &value) != CODES_SUCCESS) {
		throw InvalidInputException(std::string(function) + ": GRIB key '" + key + "' is missing");
	}
	return value;
}

static long Grib2LongKeyOrDefault(codes_handle *handle, const char *key, long default_value) {
	long value = 0;
	if (codes_get_long(handle, key, &value) != CODES_SUCCESS) {
		return default_value;
	}
	return value;
}

static double Grib2DoubleKey(codes_handle *handle, const char *key, const char *function) {
	double value = 0;
	if (codes_get_double(handle, key, &value) != CODES_SUCCESS) {
		throw InvalidInputException(std::string(function) + ": GRIB key '" + key + "' is missing");
	}
	return value;
}

//! Create an ecCodes handle for the first message in a buffer.
static CodesHandlePtr OpenGribMessage(const char *function, const char *data, idx_t size) {
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
static std::vector<double> DecodeGribValues(codes_handle *handle, const char *function) {
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

static std::string DescribeGribMessage(const char *data, idx_t size) {
	const char *function = "grib2_message_info";
	CodesHandlePtr handle = OpenGribMessage(function, data, size);
	const std::vector<double> values = DecodeGribValues(handle.get(), function);

	std::string min_str = "NA";
	std::string max_str = "NA";
	if (!values.empty()) {
		const auto minmax = std::minmax_element(values.begin(), values.end());
		min_str = StringUtil::Format("%.3f", *minmax.first);
		max_str = StringUtil::Format("%.3f", *minmax.second);
	}

	std::string out;
	for (const char *key : {"edition", "shortName", "typeOfLevel", "level", "gridType", "Ni", "Nj", "numberOfPoints",
	                        "packingType", "alternativeRowScanning"}) {
		out += key;
		out += "=";
		out += Grib2KeyAsString(handle.get(), key);
		out += ";";
	}
	out += "values=" + std::to_string(values.size()) + ";min=" + min_str + ";max=" + max_str;
	return out;
}

struct Grib2Grid {
	std::unique_ptr<ProjectedGrid> grid;
	//! Rows are stored in alternating directions, see RawValueIndex()
	bool alternative_rows;
};

//! Build the grid described by a message's grid definition. Throws for grids that are not supported yet.
static Grib2Grid MakeGrid(codes_handle *h, const char *function) {
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
	if (grid_type == "lambert") {
		result.grid = std::make_unique<LambertGrid>(ni, nj, la1, lo1, lad, Grib2DoubleKey(h, "LoVInDegrees", function),
		                                            Grib2DoubleKey(h, "Latin1InDegrees", function),
		                                            Grib2DoubleKey(h, "Latin2InDegrees", function), dx, dy, radius);
	} else if (grid_type == "polar_stereographic") {
		if (Grib2LongKey(h, "southPoleOnProjectionPlane", function) != 0) {
			throw NotImplementedException(std::string(function) + ": south polar stereographic is not supported yet");
		}
		result.grid = std::make_unique<PolarStereographicGrid>(
		    ni, nj, la1, lo1, lad, Grib2DoubleKey(h, "orientationOfTheGridInDegrees", function), dx, dy, radius);
	} else {
		result.grid = std::make_unique<MercatorGrid>(ni, nj, la1, lo1, lad, dx, dy, radius);
	}
	return result;
}

static optional<double> PointValueFromMessage(const char *data, idx_t size, double lat, double lon) {
	const char *function = "grib2_point_value";
	CodesHandlePtr handle = OpenGribMessage(function, data, size);
	codes_handle *h = handle.get();

	const Grib2Grid grib_grid = MakeGrid(h, function);
	const ProjectedGrid &grid = *grib_grid.grid;
	GridCell cell;
	if (!grid.NearestCell(lat, lon, cell)) {
		return optional<double>();
	}

	const std::vector<double> values = DecodeGribValues(h, function);
	const int64_t cell_count = grid.Ni() * grid.Nj();
	if (static_cast<int64_t>(values.size()) != cell_count) {
		throw InvalidInputException(std::string(function) + ": the message has " + std::to_string(values.size()) +
		                            " values but its grid has " + std::to_string(cell_count) + " cells");
	}
	const double value =
	    values[static_cast<size_t>(RawValueIndex(grid.Ni(), cell.i, cell.j, grib_grid.alternative_rows))];

	// Cells masked out by a bitmap come back as the message's missing value
	if (Grib2LongKeyOrDefault(h, "bitmapPresent", 0) != 0 && value == Grib2DoubleKey(h, "missingValue", function)) {
		return optional<double>();
	}
	return value;
}

inline void Grib2MessageInfoScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t blob) {
		return StringVector::AddString(result, DescribeGribMessage(blob.GetData(), blob.GetSize()));
	});
}

inline void Grib2PointValueScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<string_t, double, double, double>(
	    args.data[0], args.data[1], args.data[2], result,
	    [&](string_t blob, double lat, double lon) -> optional<double> {
		    return PointValueFromMessage(blob.GetData(), blob.GetSize(), lat, lon);
	    });
}

static void LoadInternal(ExtensionLoader &loader) {
	// TODO: register grib2_index() and read_grib2() table functions here.
	auto eccodes_version_function =
	    ScalarFunction("grib2_eccodes_version", {}, LogicalType::VARCHAR, Grib2EccodesVersionScalarFun);
	loader.RegisterFunction(eccodes_version_function);

	// Temporary scaffolding (see the comment above the implementations)
	auto message_info_function =
	    ScalarFunction("grib2_message_info", {LogicalType::BLOB}, LogicalType::VARCHAR, Grib2MessageInfoScalarFun);
	message_info_function.SetFallible();
	loader.RegisterFunction(message_info_function);

	auto point_value_function =
	    ScalarFunction("grib2_point_value", {LogicalType::BLOB, LogicalType::DOUBLE, LogicalType::DOUBLE},
	                   LogicalType::DOUBLE, Grib2PointValueScalarFun);
	point_value_function.SetFallible();
	loader.RegisterFunction(point_value_function);
}

void Grib2Extension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string Grib2Extension::Name() {
	return "grib2";
}

std::string Grib2Extension::Version() const {
#ifdef EXT_VERSION_GRIB2
	return EXT_VERSION_GRIB2;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(grib2, loader) {
	duckdb::LoadInternal(loader);
}
}
