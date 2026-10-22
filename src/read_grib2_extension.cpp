#define DUCKDB_EXTENSION_MAIN

#include "read_grib2_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

// OpenSSL linked through vcpkg
#include <openssl/opensslv.h>

namespace duckdb {

inline void ReadGrib2ScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "...........🦆 " + name.GetString());
	});
}

inline void ReadGrib2OpenSSLVersionScalarFun(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &name_vector = args.data[0];
	UnaryExecutor::Execute<string_t, string_t>(name_vector, result, args.size(), [&](string_t name) {
		return StringVector::AddString(result, "ReadGrib2 " + name.GetString() + ", my linked OpenSSL version is " +
		                                           OPENSSL_VERSION_TEXT);
	});
}

static void LoadInternal(ExtensionLoader &loader) {
	// Register a scalar function
	auto read_grib2_scalar_function =
	    ScalarFunction("read_grib2", {LogicalType::VARCHAR}, LogicalType::VARCHAR, ReadGrib2ScalarFun);

	loader.RegisterFunction(read_grib2_scalar_function);

	// Register another scalar function
	auto read_grib2_openssl_version_scalar_function = ScalarFunction("read_grib2_openssl_version", {LogicalType::VARCHAR},
	                                                             LogicalType::VARCHAR, ReadGrib2OpenSSLVersionScalarFun);
	loader.RegisterFunction(read_grib2_openssl_version_scalar_function);
}

void ReadGrib2Extension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}
std::string ReadGrib2Extension::Name() {
	return "read_grib2";
}

std::string ReadGrib2Extension::Version() const {
#ifdef EXT_VERSION_READ_GRIB2
	return EXT_VERSION_READ_GRIB2;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(read_grib2, loader) {
	duckdb::LoadInternal(loader);
}
}
