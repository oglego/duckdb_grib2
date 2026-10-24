"""Regenerate test/data/sample.grib2.

A tiny synthetic GRIB2 message (16 x 31 regular lat/lon grid, 496 values from 250.0 to 299.5)
using complex packing with spatial differencing, the template family used by many NWP products.

Requires: pip install eccodes ecmwflibs
"""

import eccodes as ec

handle = ec.codes_grib_new_from_samples("regular_ll_sfc_grib2")
ec.codes_set(handle, "dataDate", 20260101)
ec.codes_set(handle, "dataTime", 0)
ec.codes_set(handle, "packingType", "grid_complex_spatial_differencing")
ec.codes_set(handle, "bitsPerValue", 16)
count = ec.codes_get(handle, "numberOfPoints")
ec.codes_set_values(handle, [250.0 + 0.1 * i for i in range(count)])
with open("test/data/sample.grib2", "wb") as out:
    ec.codes_write(handle, out)
ec.codes_release(handle)
