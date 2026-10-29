#define DUCKDB_EXTENSION_MAIN

#include "grib2_extension.hpp"
#include "grib2_codes.hpp"
#include "grib2_grid.hpp"
#include "grib2_index_function.hpp"
#include "grib2_read_function.hpp"
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
static std::string DescribeGribMessage(const char *data, idx_t size) {
	const char *function = "grib2_message_info";
	CodesHandlePtr handle = OpenGribMessage(function, data, size);
	const std::vector<double> values = DecodeGribValues(handle.get(), function);

	// Cells masked out by a bitmap come back as the missing value; leave them out of min and max
	const bool has_bitmap = Grib2LongKeyOrDefault(handle.get(), "bitmapPresent", 0) != 0;
	const double missing_value = Grib2OptionalDouble(handle.get(), "missingValue").value_or(9999.0);
	bool any_value = false;
	double min_value = 0;
	double max_value = 0;
	for (const double value : values) {
		if (has_bitmap && value == missing_value) {
			continue;
		}
		if (!any_value) {
			min_value = max_value = value;
			any_value = true;
		} else {
			min_value = std::min(min_value, value);
			max_value = std::max(max_value, value);
		}
	}
	const std::string min_str = any_value ? StringUtil::Format("%.3f", min_value) : "NA";
	const std::string max_str = any_value ? StringUtil::Format("%.3f", max_value) : "NA";

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

	RegisterGrib2IndexFunction(loader);
	RegisterGrib2ReadFunction(loader);

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
