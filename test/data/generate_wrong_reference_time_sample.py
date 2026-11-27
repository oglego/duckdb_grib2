"""Writes test/data/wrong_reference_time.grib2 and its .idx: one real HRRR message and an inventory with the right
offsets and lengths but a reference time (d=) that is not the message's.

This is what a sidecar from another run of the same file looks like when the messages happen to be the same size, which
a length check alone cannot detect. The extension must refuse it.

Pure Python; run from the repository root: python3 test/data/generate_wrong_reference_time_sample.py
"""

SOURCE = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
WRONG_REFERENCE_TIME = "2020010100"

message = open(SOURCE, "rb").read()
assert message[:4] == b"GRIB" and message[-4:] == b"7777", SOURCE
open("test/data/wrong_reference_time.grib2", "wb").write(message)
open("test/data/wrong_reference_time.grib2.idx", "w").write(
    "1:0:d=%s:CSNOW:surface:1 hour fcst:\n" % WRONG_REFERENCE_TIME
)
print("wrong_reference_time.grib2:", len(message), "bytes, idx says", WRONG_REFERENCE_TIME)
