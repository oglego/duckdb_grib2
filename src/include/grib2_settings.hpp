#pragma once

//! Extension settings and runtime tuning for GRIB2 I/O.
//!
//! By default every file read happens on the query's own thread. That is the safest behaviour: DuckDB's file-system
//! layer (and filesystem extensions such as httpfs, with their secrets and credential lookups) is then only ever called
//! from the thread that owns the query.
//!
//! For remote files, round trips dominate the run time, and the extension can overlap them: with grib2_io_threads above
//! 1, grib2_index indexes several files at once and read_grib2 keeps several message fetches in flight while it decodes
//! the current message. Those background threads call DuckDB's file system from outside DuckDB's scheduler, so the
//! overlap is opt-in. Results are identical either way.
//!
//! Example:
//!   SET grib2_io_threads = 8;   -- opt in to overlapped reads (1, the default, keeps all reads on the query thread)
//!
//! For read_grib2 the value is also the number of message fetches kept in flight, capped at
//! GRIB2_MAX_PREFETCH_MESSAGES.

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

constexpr int64_t GRIB2_DEFAULT_IO_THREADS = 1;
constexpr int64_t GRIB2_MAX_IO_THREADS = 64;
//! Most message fetches read_grib2 keeps in flight, however high grib2_io_threads is set
constexpr int64_t GRIB2_MAX_PREFETCH_MESSAGES = 6;

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
	config.AddExtensionOption("grib2_io_threads",
	                          "Number of concurrent file reads grib2_index and read_grib2 may overlap (1 to " +
	                              std::to_string(GRIB2_MAX_IO_THREADS) +
	                              "). 1 keeps all reads on the query thread. Values above 1 use " +
	                              "background threads, which helps with remote files. Default " +
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
