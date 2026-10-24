#define DUCKDB_EXTENSION_MAIN

#include "grib2_extension.hpp"
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
// Decode spike: grib2_message_info(BLOB) -> VARCHAR
//
// Decodes the first GRIB message found in an in-memory buffer and returns a
// short description. This exists to validate, from inside the extension:
//   - ecCodes definitions are available at runtime (key lookups work),
//   - the packing template of the message can be decoded (values are read).
// It is a throwaway; it will be replaced by grib2_index() / read_grib2().
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

static std::string DescribeGribMessage(const char *data, idx_t size) {
	// Cheap pre-check: a GRIB message starts with the 4-byte "GRIB" marker followed by the rest of the indicator
	// section. This also keeps ecCodes from logging errors to stderr for obviously non-GRIB input.
	if (size < 16 || std::memcmp(data, "GRIB", 4) != 0) {
		throw InvalidInputException("grib2_message_info: input is not a valid GRIB message");
	}

	// The _copy variant makes ecCodes own its buffer, so the DuckDB vector data can be released safely.
	CodesHandlePtr handle(codes_handle_new_from_message_copy(nullptr, data, static_cast<size_t>(size)));
	if (!handle) {
		throw InvalidInputException("grib2_message_info: input is not a valid GRIB message");
	}

	size_t value_count = 0;
	if (codes_get_size(handle.get(), "values", &value_count) != CODES_SUCCESS) {
		throw InvalidInputException("grib2_message_info: could not determine the number of values");
	}
	std::vector<double> values(value_count);
	if (value_count > 0 &&
	    codes_get_double_array(handle.get(), "values", values.data(), &value_count) != CODES_SUCCESS) {
		throw InvalidInputException("grib2_message_info: failed to decode values (unsupported packing?)");
	}

	std::string min_str = "NA";
	std::string max_str = "NA";
	if (value_count > 0) {
		const auto minmax = std::minmax_element(values.begin(), values.begin() + value_count);
		min_str = StringUtil::Format("%.3f", *minmax.first);
		max_str = StringUtil::Format("%.3f", *minmax.second);
	}

	std::string out;
	for (const char *key :
	     {"edition", "shortName", "typeOfLevel", "level", "gridType", "Ni", "Nj", "numberOfPoints", "packingType"}) {
		out += key;
		out += "=";
		out += Grib2KeyAsString(handle.get(), key);
		out += ";";
	}
	out += "values=" + std::to_string(value_count) + ";min=" + min_str + ";max=" + max_str;
	return out;
}

inline void Grib2MessageInfoScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t blob) {
		return StringVector::AddString(result, DescribeGribMessage(blob.GetData(), blob.GetSize()));
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// TODO: register grib2_index() and read_grib2() table functions here.
	auto eccodes_version_function =
	    ScalarFunction("grib2_eccodes_version", {}, LogicalType::VARCHAR, Grib2EccodesVersionScalarFun);
	loader.RegisterFunction(eccodes_version_function);

	// Decode spike (temporary)
	auto message_info_function =
	    ScalarFunction("grib2_message_info", {LogicalType::BLOB}, LogicalType::VARCHAR, Grib2MessageInfoScalarFun);
	message_info_function.SetFallible();
	loader.RegisterFunction(message_info_function);
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
