"""Regenerate test/data/multi_field.grib2: one GRIB2 message that holds two fields.

GRIB2 allows a message to repeat sections 4 to 7, so one message can carry several fields that share a grid. The source is
the smallest real message in fixtures/ (188 bytes, one field). Its sections 4 to 7 are appended once more before the end
section and the total length in section 0 is updated, giving a well-formed two-field message. ecCodes decodes only the
first field of such a message, so the extension refuses it rather than silently dropping the second field.

Run from the repository root. No dependencies.
"""

SRC = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
OUT = "test/data/multi_field.grib2"

data = bytearray(open(SRC, "rb").read())
assert data[:4] == b"GRIB" and data[-4:] == b"7777"
pos, field_start = 16, None
while pos + 5 <= len(data) - 4:
    length = int.from_bytes(data[pos : pos + 4], "big")
    if data[pos + 4] == 4:
        field_start = pos
        break
    pos += length
assert field_start is not None, "no product definition section found"
field = bytes(data[field_start : len(data) - 4])  # sections 4 to 7
out = bytes(data[: len(data) - 4]) + field + b"7777"
out = bytearray(out)
out[8:16] = len(out).to_bytes(8, "big")
open(OUT, "wb").write(bytes(out))
print("wrote %s (%d bytes), two fields of %d bytes each after the header" % (OUT, len(out), len(field)))
