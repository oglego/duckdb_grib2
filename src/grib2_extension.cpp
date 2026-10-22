#define DUCKDB_EXTENSION_MAIN

#include "grib2_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// ecCodes linked through vcpkg
#include <eccodes.h>

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

static void LoadInternal(ExtensionLoader &loader) {
	// TODO: register grib2_index() and read_grib2() table functions here.
	auto eccodes_version_function =
	    ScalarFunction("grib2_eccodes_version", {}, LogicalType::VARCHAR, Grib2EccodesVersionScalarFun);
	loader.RegisterFunction(eccodes_version_function);
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
