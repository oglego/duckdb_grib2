"""Writes test/data/sections_out_of_order.grib2: the HRRR single-field fixture with its sections laid out 5, 7, 6, 7 (a
data section before the bitmap section it belongs after).

ecCodes decodes such a message, but it is not laid out as the standard says and the layout checks were not written for
it, so the extension refuses it. Pure Python; run from the repository root:
    python3 test/data/generate_section_order_sample.py
"""

SOURCE = "fixtures/hrrr_msg90_csnow_simple_const.grib2"

message = open(SOURCE, "rb").read()
assert message[:4] == b"GRIB" and message[-4:] == b"7777", SOURCE

sections = {}
position = 16
while message[position : position + 4] != b"7777":
    length = int.from_bytes(message[position : position + 4], "big")
    sections[message[position + 4]] = message[position : position + length]
    position += length

body = (
    b"".join(sections[n] for n in (1, 3, 4, 5)) if 2 not in sections else b"".join(sections[n] for n in (1, 2, 3, 4, 5))
)
body += sections[7] + sections[6] + sections[7] + b"7777"
out = message[:8] + (16 + len(body)).to_bytes(8, "big") + body
open("test/data/sections_out_of_order.grib2", "wb").write(out)
print("sections_out_of_order.grib2:", len(out), "bytes; sections", sorted(sections))
