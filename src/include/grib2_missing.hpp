#pragma once

//! How the extension tells cells that carry no data from cells that do.
//!
//! ecCodes writes a "missing value" marker into every cell that has no data, and by default that marker is 9999. Telling
//! such cells from real ones by comparing against 9999 is wrong in two ways:
//!   * A message can mark cells missing without a bitmap: complex packing (templates 5.2 and 5.3) with missing value
//!     management has the packed data itself flag them. ecCodes then fills those cells with 9999 too, and a decoder
//!     that only looks at messages with a bitmap returns them as real values.
//!   * A real value of exactly 9999 (for example a visibility in metres) is indistinguishable from the marker.
//! So the decoder asks ecCodes to use NaN as the marker. GRIB2 packing cannot store a NaN (the packed values are
//! integers, and the reference value is a float that ecCodes decodes to a number), so a NaN in the decoded values means
//! "no data" and nothing else, whatever the packing or bitmap. This header needs only ecCodes, so the standalone tests
//! can exercise exactly the code that read_grib2 uses.

#include <eccodes.h>

#include <cmath>
#include <limits>

//! Ask ecCodes to use NaN for cells with no data. Must be called before the values are read. False when ecCodes refused
//! the key; the caller must not decode in that case, because the values would carry the default marker.
inline bool Grib2UseNanForMissing(codes_handle *handle) {
	return codes_set_double(handle, "missingValue", std::numeric_limits<double>::quiet_NaN()) == CODES_SUCCESS;
}

//! True for a cell with no data, in values decoded after Grib2UseNanForMissing
inline bool Grib2IsMissingValue(double value) {
	return std::isnan(value);
}
