"""Regenerate test/data/degenerate_lambert.grib2: a real HRRR message whose Lambert standard parallels mirror each other.

The source is the smallest real message in fixtures/ (188 bytes, a constant field on the HRRR Lambert conformal grid,
Latin1 = Latin2 = 38.5 degrees). Only the sign of Latin2 (section 3, octets 70-73, sign-and-magnitude in millionths of a
degree) is changed, giving Latin1 = 38.5 and Latin2 = -38.5. The cone constant of such a grid is zero, so the projection
formulas divide by zero. The extension must refuse to build the grid instead of returning NaN coordinates.

Run from the repository root. No dependencies.
"""

SRC = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
OUT = "test/data/degenerate_lambert.grib2"

data = bytearray(open(SRC, "rb").read())
assert data[:4] == b"GRIB" and data[-4:] == b"7777"
pos, changed = 16, False
while pos + 5 <= len(data) - 4:
    length = int.from_bytes(data[pos : pos + 4], "big")
    number = data[pos + 4]
    if number == 3:
        template = int.from_bytes(data[pos + 12 : pos + 14], "big")
        assert template == 30, "expected grid template 3.30 (Lambert conformal), found %d" % template
        latin1 = int.from_bytes(data[pos + 65 : pos + 69], "big")
        latin2 = int.from_bytes(data[pos + 69 : pos + 73], "big")
        assert latin1 == latin2 and latin1 < 0x80000000, "expected equal, positive standard parallels"
        data[pos + 69] |= 0x80  # sign bit: Latin2 becomes -Latin1
        changed = True
        break
    pos += length
assert changed, "no grid definition section found"
open(OUT, "wb").write(bytes(data))
print("wrote %s (%d bytes), Latin1 = %.6f, Latin2 = %.6f" % (OUT, len(data), latin1 / 1e6, -latin2 / 1e6))
