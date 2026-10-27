"""Regenerate test/data/masked_gu.grib2: the Guam fixture with a bitmap masking a block of cells.

Real NBM messages do not always have bitmaps, and the CONUS fixtures here have none, so this synthetic message is the
way to test cells that are masked out. Cells i in 60..79, j in 100..119 (canonical grid coordinates, i east and j
north) are masked. ecCodes returns its missing value (9999) for them; the extension must report them as NULL and
leave them out of min and max.

Requires: pip install eccodes ecmwflibs numpy
Run from the repository root.
"""

import numpy as np
import eccodes as ec

SRC = "fixtures/nbm_gu_msg.grib2"
OUT = "test/data/masked_gu.grib2"
MISSING = 9999.0
I_RANGE = range(60, 80)
J_RANGE = range(100, 120)

h = ec.codes_new_from_message(open(SRC, "rb").read())
ni, nj = ec.codes_get(h, "Ni"), ec.codes_get(h, "Nj")
alt = int(ec.codes_get(h, "alternativeRowScanning"))
assert ec.codes_get(h, "bitmapPresent") == 0
values = np.asarray(ec.codes_get_double_array(h, "values")).copy()  # ecCodes' stored row order

for j in J_RANGE:
    for i in I_RANGE:
        values[j * ni + ((ni - 1 - i) if (alt and j % 2) else i)] = MISSING

ec.codes_set(h, "bitmapPresent", 1)
ec.codes_set(h, "missingValue", MISSING)
ec.codes_set_values(h, values)
with open(OUT, "wb") as out:
    ec.codes_write(h, out)

# Read it back and report what a decoder will see
check = ec.codes_new_from_message(open(OUT, "rb").read())
back = np.asarray(ec.codes_get_double_array(check, "values"))
print(
    "wrote %s: packing=%s bitmapPresent=%d masked=%d numberOfValues=%d numberOfDataPoints=%d"
    % (
        OUT,
        ec.codes_get(check, "packingType"),
        ec.codes_get(check, "bitmapPresent"),
        int((back == MISSING).sum()),
        ec.codes_get(check, "numberOfValues"),
        ec.codes_get(check, "numberOfDataPoints"),
    )
)
