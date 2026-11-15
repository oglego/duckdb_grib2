"""Regenerate test/data/huge_count.grib2: a real HRRR message whose grid definition claims 300 million points.

The source is the smallest real message in fixtures/ (188 bytes, a constant field). Only the "number of data points" field
of the grid definition section (section 3, octets 7-10) is changed. Nothing in GRIB2 checksums the message, so the result
parses as a well-formed message with an absurd size claim, like a corrupt or hostile file would. The extension must
refuse to decode it (the claim exceeds the limit, and ecCodes sizes buffers from such counts) and must not pass it to
ecCodes for grib2_index(..., details := true).

Run from the repository root. No dependencies.
"""

SRC = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
OUT = "test/data/huge_count.grib2"
CLAIMED_POINTS = 300_000_000

data = bytearray(open(SRC, "rb").read())
assert data[:4] == b"GRIB" and data[-4:] == b"7777"
pos, changed = 16, False
while pos + 5 <= len(data) - 4:
    length = int.from_bytes(data[pos : pos + 4], "big")
    number = data[pos + 4]
    if number == 3:
        data[pos + 6 : pos + 10] = CLAIMED_POINTS.to_bytes(4, "big")
        changed = True
        break
    pos += length
assert changed, "no grid definition section found"
open(OUT, "wb").write(bytes(data))
print("wrote %s (%d bytes), grid definition claims %d points" % (OUT, len(data), CLAIMED_POINTS))
