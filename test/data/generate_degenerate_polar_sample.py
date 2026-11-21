"""Regenerate test/data/degenerate_polar.grib2: a small north polar stereographic message with LaD = -90 degrees.

North polar stereographic scales by 1 + sin(LaD), which is zero at LaD = -90: every cell would project onto one point.
The extension must refuse the grid instead of returning cells at a bogus position. The grid is a 20 x 20 window of the
NBM Alaska grid, re-encoded with simple packing, and only LaD is changed.

Requires: pip install eccodes numpy
Run from the repository root.
"""

import numpy as np
import eccodes as ec

SRC = "fixtures/nbm_ak_msg.grib2"
OUT = "test/data/degenerate_polar.grib2"

h = ec.codes_new_from_message(open(SRC, "rb").read())
assert ec.codes_get(h, "gridType") == "polar_stereographic"
ec.codes_set(h, "packingType", "grid_simple")
ec.codes_set(h, "alternativeRowScanning", 0)
ec.codes_set(h, "Ni", 20)
ec.codes_set(h, "Nj", 20)
ec.codes_set(h, "numberOfDataPoints", 400)
ec.codes_set(h, "numberOfValues", 400)
ec.codes_set(h, "LaD", -90000000)
ec.codes_set_values(h, np.linspace(250.0, 290.0, 400))
with open(OUT, "wb") as out:
    ec.codes_write(h, out)

check = ec.codes_new_from_message(open(OUT, "rb").read())
print(
    "wrote %s: Ni=%d Nj=%d LaD=%.6f packing=%s"
    % (
        OUT,
        ec.codes_get(check, "Ni"),
        ec.codes_get(check, "Nj"),
        ec.codes_get(check, "LaDInDegrees"),
        ec.codes_get(check, "packingType"),
    )
)
