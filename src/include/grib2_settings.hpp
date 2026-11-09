#pragma once

// Extension settings.
//
//   SET grib2_io_threads = 8;   -- 1 turns off the extension's own background threads
//
// read_grib2 and grib2_index overlap network round trips by reading from a few background threads. That is a large win
// on remote files, but those threads call DuckDB's file system from outside DuckDB's scheduler. A value of 1 keeps
// every read on the query's own thread (slower on remote files, identical results), which is the setting to reach for
// if a filesystem extension turns out not to cope, or in an environment that restricts thread creation.

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

constexpr int64_t GRIB2_DEFAULT_IO_THREADS = 8;
constexpr int64_t GRIB2_MAX_IO_THREADS = 64;

//! Validate on SET, so a bad value is reported where it is set rather than on the next query
inline void ValidateIoThreads(ClientContext &context, SetScope scope, Value &parameter) {
	if (parameter.IsNull()) {
		throw InvalidInputException("grib2_io_threads cannot be NULL");
	}
	const int64_t threads = parameter.GetValue<int64_t>();
	if (threads < 1 || threads > GRIB2_MAX_IO_THREADS) {
		throw InvalidInputException("grib2_io_threads must be between 1 and " + std::to_string(GRIB2_MAX_IO_THREADS));
	}
}

inline void RegisterGrib2Settings(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(
	    "grib2_io_threads",
	    "Number of background threads grib2_index and read_grib2 use to overlap file reads (1 to " +
	        std::to_string(GRIB2_MAX_IO_THREADS) + "). 1 keeps all reads on the query thread. Default " +
	        std::to_string(GRIB2_DEFAULT_IO_THREADS) + ".",
	    LogicalType::BIGINT, Value::BIGINT(GRIB2_DEFAULT_IO_THREADS), ValidateIoThreads);
}

//! The grib2_io_threads setting, validated
inline idx_t Grib2IoThreads(ClientContext &context) {
	int64_t threads = GRIB2_DEFAULT_IO_THREADS;
	Value value;
	auto lookup = context.TryGetCurrentSetting("grib2_io_threads", value);
	if (lookup && !value.IsNull()) {
		threads = value.GetValue<int64_t>();
	}
	if (threads < 1 || threads > GRIB2_MAX_IO_THREADS) {
		throw InvalidInputException("grib2_io_threads must be between 1 and " + std::to_string(GRIB2_MAX_IO_THREADS));
	}
	return static_cast<idx_t>(threads);
}

} // namespace duckdb
