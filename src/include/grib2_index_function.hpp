#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Registers grib2_index(path, idx := true, details := false)
void RegisterGrib2IndexFunction(ExtensionLoader &loader);

} // namespace duckdb
