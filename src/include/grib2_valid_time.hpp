#pragma once

//! The time a GRIB2 field is valid, read from the message itself.
//!
//! ecCodes derives validityDate (YYYYMMDD) and validityTime (HHMM) from section 1 and the product definition (section
//! 4): the forecast time and its unit for an instantaneous field, the end of the interval for an accumulation, maximum
//! or average, and so on for every product template it knows. Reading them means valid_time does not depend on an .idx
//! sidecar, and it also covers steps the inventory text cannot express, such as `15 min fcst` or `0-1 day acc fcst`.
//!
//! This header does not include DuckDB, so test/cpp/valid_time_check.cpp can compile it alone.

#include <eccodes.h>

#include <cstdint>
#include <optional>

namespace duckdb {

//! Days from 1970-01-01 to a date in the proleptic Gregorian calendar (negative before 1970)
inline int64_t Grib2DaysFromCivil(int64_t year, int month, int day) {
	year -= month <= 2 ? 1 : 0;
	const int64_t era = (year >= 0 ? year : year - 399) / 400;
	const int64_t year_of_era = year - era * 400;
	const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
	return era * 146097 + day_of_era - 719468;
}

inline bool Grib2IsLeapYear(int64_t year) {
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

//! Microseconds since the Unix epoch (UTC) for a calendar date and time, or nothing when a field is out of range.
//! Years 1 to 9999, so the value always fits a DuckDB TIMESTAMP.
inline std::optional<int64_t> Grib2MakeMicros(int64_t year, int month, int day, int hour, int minute) {
	static const int days_in_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	if (year < 1 || year > 9999 || month < 1 || month > 12 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
		return std::nullopt;
	}
	const int month_length = days_in_month[month - 1] + (month == 2 && Grib2IsLeapYear(year) ? 1 : 0);
	if (day < 1 || day > month_length) {
		return std::nullopt;
	}
	const int64_t seconds = Grib2DaysFromCivil(year, month, day) * 86400 + hour * 3600 + minute * 60;
	return seconds * 1000000;
}

//! The valid time of the field in microseconds since the epoch, to the minute (validityTime has no seconds), or nothing
//! when ecCodes cannot work it out for this product template or reports a date that does not exist.
inline std::optional<int64_t> Grib2ValidTimeMicros(codes_handle *handle) {
	long date = 0;
	long time = 0;
	if (codes_get_long(handle, "validityDate", &date) != CODES_SUCCESS ||
	    codes_get_long(handle, "validityTime", &time) != CODES_SUCCESS || date < 0 || time < 0) {
		return std::nullopt;
	}
	return Grib2MakeMicros(date / 10000, static_cast<int>((date / 100) % 100), static_cast<int>(date % 100),
	                       static_cast<int>(time / 100), static_cast<int>(time % 100));
}

} // namespace duckdb
