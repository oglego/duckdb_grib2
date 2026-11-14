"""Regenerate the missing-value fixtures in test/data/, all derived from the Guam fixture (fixtures/nbm_gu_msg.grib2).

A cell with no data can be marked in two ways, and ecCodes fills both with its default missing value, 9999:

  missing_complex.grib2  template 5.2 (complex packing) with missing value management and NO bitmap: the packed data
  missing_spatial.grib2  template 5.3 (complex + spatial differencing), same: the packed data flags the missing cells
  real_9999.grib2        a bitmap masks the same block, and every other cell is a real value of exactly 9999.0

The first two catch a decoder that only treats cells as missing when the message has a bitmap. The third catches a
decoder that compares against 9999 to find missing cells. In all three, the block i in 60..79, j in 100..119 (canonical
grid coordinates, i east and j north) has no data: 400 cells that the extension must report as NULL.

Requires: pip install eccodes numpy
Run from the repository root.
"""

import numpy as np
import eccodes as ec

SRC = "fixtures/nbm_gu_msg.grib2"
I_RANGE = range(60, 80)
J_RANGE = range(100, 120)
SENTINEL = 9999.0  # ecCodes' default; also used as the input marker for the two packing cases
OTHER_SENTINEL = -1.0e30  # real_9999 must use a marker that is not 9999 while encoding


def load():
    h = ec.codes_new_from_message(open(SRC, "rb").read())
    ni = ec.codes_get(h, "Ni")
    alt = int(ec.codes_get(h, "alternativeRowScanning"))
    assert ec.codes_get(h, "bitmapPresent") == 0
    values = np.asarray(ec.codes_get_double_array(h, "values")).copy()  # ecCodes' stored row order
    return h, ni, alt, values


def masked_indexes(ni, alt):
    return [j * ni + ((ni - 1 - i) if (alt and j % 2) else i) for j in J_RANGE for i in I_RANGE]


def write(h, out):
    with open(out, "wb") as f:
        ec.codes_write(h, f)


def report(out):
    h = ec.codes_new_from_message(open(out, "rb").read())
    ec.codes_set(h, "missingValue", float("nan"))
    v = np.asarray(ec.codes_get_double_array(h, "values"))
    print(
        "wrote %s: template=5.%d bitmapPresent=%d missingValueManagementUsed=%s nan=%d eq9999=%d cells=%d"
        % (
            out,
            ec.codes_get(h, "dataRepresentationTemplateNumber"),
            ec.codes_get(h, "bitmapPresent"),
            ec.codes_get(h, "missingValueManagementUsed") if ec.codes_get(h, "dataRepresentationTemplateNumber") != 0 else "-",
            int(np.isnan(v).sum()),
            int((v == 9999.0).sum()),
            v.size,
        )
    )


def packed_with_missing(packing, out):
    """Complex packing; the cells with no data are flagged by the packing, not by a bitmap."""
    h, ni, alt, values = load()
    values[masked_indexes(ni, alt)] = SENTINEL
    ec.codes_set(h, "packingType", packing)
    ec.codes_set(h, "missingValueManagementUsed", 1)
    ec.codes_set(h, "bitsPerValue", 16)
    ec.codes_set(h, "bitmapPresent", 0)
    ec.codes_set(h, "missingValue", SENTINEL)
    ec.codes_set_values(h, values)
    assert ec.codes_get(h, "bitmapPresent") == 0, "ecCodes added a bitmap; the fixture would not test the intended case"
    write(h, out)
    report(out)


def real_9999(out):
    """A bitmap masks the block; every other cell is a real value of exactly 9999.0 (a constant field packs exactly)."""
    h, ni, alt, values = load()
    values[:] = SENTINEL
    values[masked_indexes(ni, alt)] = OTHER_SENTINEL
    ec.codes_set(h, "bitmapPresent", 1)
    ec.codes_set(h, "missingValue", OTHER_SENTINEL)
    ec.codes_set_values(h, values)
    write(h, out)
    report(out)


packed_with_missing("grid_complex", "test/data/missing_complex.grib2")
packed_with_missing("grid_complex_spatial_differencing", "test/data/missing_spatial.grib2")
real_9999("test/data/real_9999.grib2")
