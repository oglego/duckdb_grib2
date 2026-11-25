"""Writes test/data/grown_after_idx.grib2 and grown_after_idx.grib2.idx: two real HRRR messages followed by a newline,
with an .idx written for the file before the newline was added.

The .idx is right about both offsets, so message 1 checks out. The last message's length comes from the end of the file,
so it is now one byte longer than the message really is. Checking the last message as well as the first is what finds
a sidecar that does not match a file that has grown or shrunk since. A plain grib2_index listing must refuse it.

Run from the repository root. No dependencies.
"""

FIRST = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
SECOND = "fixtures/hrrr_msg166_icec_spatial.grib2"

first = open(FIRST, "rb").read()
second = open(SECOND, "rb").read()
open("test/data/grown_after_idx.grib2", "wb").write(first + second + b"\n")
open("test/data/grown_after_idx.grib2.idx", "w").write(
    "1:0:d=2026060200:CSNOW:surface:anl:\n2:%d:d=2026060200:ICEC:surface:anl:\n" % len(first)
)
print("wrote grown_after_idx.grib2 (%d bytes) and its .idx" % (len(first) + len(second) + 1))
