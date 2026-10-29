#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Registers read_grib2(path, message := ..., search := ..., points := ..., bbox := ..., all_cells := false)
void RegisterGrib2ReadFunction(ExtensionLoader &loader);

} // namespace duckdb
