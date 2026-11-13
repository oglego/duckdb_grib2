#pragma once

// Helpers shared by the table functions for writing optional values into output vectors.

#include "duckdb.hpp"

#include <string>

namespace duckdb {

//! Write an optional value, as NULL when it is absent
template <class T>
inline void WriteOptional(VectorWriter<T> &writer, const optional<T> &value) {
	if (value) {
		writer.WriteValue(*value);
	} else {
		writer.WriteNull();
	}
}

inline void WriteString(VectorWriter<string_t> &writer, const std::string &value) {
	writer.WriteValue(string_t(value.c_str(), UnsafeNumericCast<uint32_t>(value.size())));
}

inline void WriteOptionalString(VectorWriter<string_t> &writer, const optional<std::string> &value) {
	if (value) {
		WriteString(writer, *value);
	} else {
		writer.WriteNull();
	}
}

} // namespace duckdb
