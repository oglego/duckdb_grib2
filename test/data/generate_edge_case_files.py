"""Regenerate test/data/empty.grib2 and test/data/trailing_bytes.grib2.

  empty.grib2           a zero-byte file: usually a failed download, so it is an error, not "no messages"
  trailing_bytes.grib2  one real HRRR message (fixtures/hrrr_msg90_csnow_simple_const.grib2) followed by a newline: too few
                        bytes for another message, reported as trailing data rather than as a truncated message

Run from the repository root. No dependencies.
"""

SRC = "fixtures/hrrr_msg90_csnow_simple_const.grib2"

open("test/data/empty.grib2", "wb").close()
message = open(SRC, "rb").read()
assert message[:4] == b"GRIB" and message[-4:] == b"7777"
open("test/data/trailing_bytes.grib2", "wb").write(message + b"\n")
print("wrote test/data/empty.grib2 (0 bytes) and test/data/trailing_bytes.grib2 (%d bytes)" % (len(message) + 1))
