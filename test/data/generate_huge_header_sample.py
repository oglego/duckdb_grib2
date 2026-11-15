"""Regenerate test/data/huge_header.grib2: a real HRRR message with a 1.1 MB local-use section.

The source is the smallest real message in fixtures/ (188 bytes). A local-use section (section 2) of 1,100,000 bytes of
zeros is inserted after the identification section and the total length in the indicator section is updated. The result
is a well-formed message whose headers (sections 0 to 5) are larger than the reader will fetch (1 MiB), like a corrupt or
hostile file whose section lengths ask for far more than any real header. grib2_index must refuse it without allocating
or reading that much.

The file is 1.1 MB of mostly zeros. Run from the repository root. No dependencies.
"""

SRC = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
OUT = "test/data/huge_header.grib2"
SECTION2_BYTES = 1_100_000

data = bytearray(open(SRC, "rb").read())
assert data[:4] == b"GRIB" and data[-4:] == b"7777"
section1_length = int.from_bytes(data[16:20], "big")
assert data[20] == 1, "expected the identification section right after the indicator section"
at = 16 + section1_length
assert data[at + 4] != 2, "the source already has a local-use section"
section2 = bytearray(SECTION2_BYTES)
section2[0:4] = SECTION2_BYTES.to_bytes(4, "big")
section2[4] = 2
data[at:at] = section2
data[8:16] = len(data).to_bytes(8, "big")
open(OUT, "wb").write(bytes(data))
print("wrote %s (%d bytes), local-use section of %d bytes" % (OUT, len(data), SECTION2_BYTES))
