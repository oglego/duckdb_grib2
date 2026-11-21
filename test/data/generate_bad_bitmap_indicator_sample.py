"""Regenerate test/data/bad_bitmap_indicator.grib2: a real HRRR message whose bitmap indicator says "reuse the previous
bitmap" (254).

The source is the smallest real message in fixtures/ (188 bytes, a constant field, no bitmap: indicator 255). Only the
bitmap indicator (section 6, octet 6) is changed. A message holds one field, so there is no earlier bitmap to reuse.
ecCodes ignores the indicator and decodes the field as if it had no bitmap, so the extension must refuse the message
rather than return unmasked values.

Run from the repository root. No dependencies.
"""

SRC = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
OUT = "test/data/bad_bitmap_indicator.grib2"

data = bytearray(open(SRC, "rb").read())
assert data[:4] == b"GRIB" and data[-4:] == b"7777"
pos, changed = 16, False
while pos + 5 <= len(data) - 4:
    length = int.from_bytes(data[pos : pos + 4], "big")
    if data[pos + 4] == 6:
        assert data[pos + 5] == 255, "expected a message with no bitmap"
        data[pos + 5] = 254
        changed = True
        break
    pos += length
assert changed, "no bitmap section found"
open(OUT, "wb").write(bytes(data))
print("wrote %s (%d bytes), bitmap indicator 254" % (OUT, len(data)))
