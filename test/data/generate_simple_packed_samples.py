"""Regenerate the template 5.0 (simple packing) fixtures in test/data/, derived from the real HRRR snow-depth message.

The one real HRRR 5.0 message in fixtures/ is a constant field, and a real 5.0 message with varying values is too large to
commit (1.9 million cells at even one bit per value is ~240 KB). So these cut a 120 x 100 window of the real Lambert grid
(fixtures/hrrr_msg70_snod_spatial.grib2, around Mount Rainier) and re-encode it with simple packing. They are synthetic
re-encodings of real values on a real grid, not messages NOAA produced.

  simple_window.grib2   5.0, no bitmap, the real snow depths (multiples of 0.001, exact at 16 bits with decimal scale 3)
  simple_bitmap.grib2   5.0 with a bitmap masking a 20 x 15 block (300 cells), real snow depths elsewhere
  simple_9999.grib2     5.0 with a bitmap masking a 20 x 15 block, integers 0..9999 elsewhere of which about one cell in
                        eleven is exactly 9999. Simple packing has no way to flag missing cells except a bitmap, and a
                        real 9999 must not be taken for the missing marker.

Block positions are in window coordinates (i east, j north from the first point; rows do not alternate in HRRR).

Requires: pip install eccodes numpy
Run from the repository root.
"""

import numpy as np
import eccodes as ec

SRC = "fixtures/hrrr_msg70_snod_spatial.grib2"
I0, J0, NI, NJ = 240, 880, 120, 100  # window of the 1799 x 1059 grid; contains the Mount Rainier maximum at (285, 920)
OTHER_SENTINEL = -1.0e30  # the marker used while encoding a bitmap; never equal to a real value

BLOCKS = {
    "simple_bitmap": (range(60, 80), range(30, 45)),
    "simple_9999": (range(20, 40), range(50, 65)),
}


def window_message():
    """A clone of the HRRR message cut down to the window, plus the real values in the window."""
    h = ec.codes_new_from_message(open(SRC, "rb").read())
    ni, nj = ec.codes_get(h, "Ni"), ec.codes_get(h, "Nj")
    assert ec.codes_get(h, "alternativeRowScanning") == 0 and ec.codes_get(h, "jScansPositively") == 1
    lat = np.asarray(ec.codes_get_double_array(h, "latitudes")).reshape(nj, ni)
    lon = np.asarray(ec.codes_get_double_array(h, "longitudes")).reshape(nj, ni)
    values = np.asarray(ec.codes_get_double_array(h, "values")).reshape(nj, ni)[J0 : J0 + NJ, I0 : I0 + NI]
    g = ec.codes_clone(h)
    ec.codes_set(g, "packingType", "grid_simple")
    ec.codes_set(g, "Ni", NI)
    ec.codes_set(g, "Nj", NJ)
    ec.codes_set(g, "latitudeOfFirstGridPointInDegrees", float(lat[J0, I0]))
    ec.codes_set(g, "longitudeOfFirstGridPointInDegrees", float(lon[J0, I0]))
    return g, values.copy()


def block_mask(name):
    mask = np.zeros((NJ, NI), dtype=bool)
    irange, jrange = BLOCKS[name]
    for j in jrange:
        for i in irange:
            mask[j, i] = True
    return mask


def write(g, out):
    with open(out, "wb") as f:
        ec.codes_write(g, f)
    h = ec.codes_new_from_message(open(out, "rb").read())
    ec.codes_set(h, "missingValue", float("nan"))
    v = np.asarray(ec.codes_get_double_array(h, "values"))
    assert v.size == NI * NJ, "number of values %d, expected %d" % (v.size, NI * NJ)
    fin = v[~np.isnan(v)]
    print(
        "wrote %s: template=5.%d bits=%d bitmapPresent=%d cells=%d nan=%d eq9999=%d nonzero=%d min=%.3f max=%.3f bytes=%d"
        % (
            out,
            ec.codes_get(h, "dataRepresentationTemplateNumber"),
            ec.codes_get(h, "bitsPerValue"),
            ec.codes_get(h, "bitmapPresent"),
            v.size,
            int(np.isnan(v).sum()),
            int((fin == 9999.0).sum()),
            int((fin > 0).sum()),
            fin.min(),
            fin.max(),
            len(open(out, "rb").read()),
        )
    )


def simple_window(out):
    g, values = window_message()
    ec.codes_set(g, "bitmapPresent", 0)
    ec.codes_set(g, "bitsPerValue", 16)
    ec.codes_set(g, "decimalScaleFactor", 3)
    ec.codes_set_values(g, values.ravel())
    decoded = np.asarray(ec.codes_get_double_array(g, "values"))
    assert np.array_equal(decoded, values.ravel()), "the window does not round-trip exactly at 16 bits"
    write(g, out)


def simple_bitmap(out):
    g, values = window_message()
    values[block_mask("simple_bitmap")] = OTHER_SENTINEL
    ec.codes_set(g, "bitmapPresent", 1)
    ec.codes_set(g, "missingValue", OTHER_SENTINEL)
    ec.codes_set(g, "bitsPerValue", 16)
    ec.codes_set(g, "decimalScaleFactor", 3)
    ec.codes_set_values(g, values.ravel())
    write(g, out)


def simple_9999(out):
    g, _ = window_message()
    j, i = np.mgrid[0:NJ, 0:NI]
    values = np.where((i * 3 + j) % 11 == 0, 9999.0, (i * 131 + j * 17) % 9999).astype(float)
    values[block_mask("simple_9999")] = OTHER_SENTINEL
    ec.codes_set(g, "bitmapPresent", 1)
    ec.codes_set(g, "missingValue", OTHER_SENTINEL)
    ec.codes_set(g, "bitsPerValue", 14)  # 9999 < 2**14
    ec.codes_set(g, "decimalScaleFactor", 0)
    ec.codes_set_values(g, values.ravel())
    write(g, out)


simple_window("test/data/simple_window.grib2")
simple_bitmap("test/data/simple_bitmap.grib2")
simple_9999("test/data/simple_9999.grib2")
