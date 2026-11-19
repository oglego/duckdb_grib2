#pragma once

// ecCodes is not thread-safe unless it was built that way: its CMake options ECCODES_THREADS (POSIX threads) and
// ECCODES_OMP_THREADS (OpenMP) both default to OFF, and distribution packages usually leave them off. This extension's
// vcpkg port turns one of them on, but the extension can also be built against another ecCodes, and DuckDB calls into
// it from several threads: the scalar functions grib2_message_info and grib2_point_value run in parallel inside one
// query, and separate connections can run read_grib2 and grib2_index at the same time.
//
// Grib2EccodesGuard is held around every use of an ecCodes handle. It asks ecCodes once whether it was built
// thread-safe; when it was, the guard does nothing, and when it was not (or cannot say), it takes one process-wide
// mutex so only a single thread is inside ecCodes at a time.
//
// This header uses only ecCodes and the standard library, so the standalone checks can include it.

#include <eccodes.h>

#include <cstddef>
#include <mutex>
#include <string>

namespace duckdb {

//! True when the linked ecCodes was built with POSIX-thread or OpenMP thread safety. False when it was not, and also
//! when it cannot say: codes_get_features exists only in newer ecCodes (2.47 has it, 2.34 does not), and the safe
//! answer for an ecCodes that cannot report is "not thread-safe".
inline bool Grib2EccodesIsThreadSafe() {
	static const bool thread_safe = [] {
#ifdef CODES_FEATURES_ENABLED
		// A space-separated list such as "MEMFS JPG ECCODES_THREADS". ecCodes writes the list before it checks the
		// length, so the buffer has to be generous; the full list of known features is under 100 characters.
		char features[1024] = {0};
		size_t length = sizeof(features);
		if (codes_get_features(features, &length, CODES_FEATURES_ENABLED) != CODES_SUCCESS) {
			return false;
		}
		const std::string list = std::string(" ") + features + " ";
		return list.find(" ECCODES_THREADS ") != std::string::npos ||
		       list.find(" ECCODES_OMP_THREADS ") != std::string::npos;
#else
		return false;
#endif
	}();
	return thread_safe;
}

//! Declare one before the CodesHandlePtr it protects, so the handle is deleted while the guard is still held. Guards
//! must not be nested: the mutex is not recursive.
class Grib2EccodesGuard {
public:
	Grib2EccodesGuard() {
		if (!Grib2EccodesIsThreadSafe()) {
			lock = std::unique_lock<std::mutex>(Mutex());
		}
	}
	Grib2EccodesGuard(const Grib2EccodesGuard &) = delete;
	Grib2EccodesGuard &operator=(const Grib2EccodesGuard &) = delete;

private:
	static std::mutex &Mutex() {
		static std::mutex mutex;
		return mutex;
	}
	std::unique_lock<std::mutex> lock;
};

} // namespace duckdb
