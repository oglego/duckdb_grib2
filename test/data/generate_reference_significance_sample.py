"""Writes test/data/reference_significance.grib2 and reference_significance_expected.tsv: four messages that differ only in
the "significance of reference time" octet of section 1 (code table 1.2).

  1  significance 1 (start of forecast), forecast time 6 hours   valid 2026-06-02 06:00
  2  significance 0 (analysis),          forecast time 0         valid 2026-06-02 00:00
  3  significance 2 (verifying time),    forecast time 6 hours   no valid time
  4  significance 3 (observation time),  forecast time 0         no valid time

For 2 and 3 the reference time is not a run time, so "reference time plus forecast time" is not the valid time even
though ecCodes computes it that way; the extension returns NULL instead of a wrong value.

Each message is the tiny HRRR fixture (a constant field on the Lambert grid).

It also writes reference_significance_with_idx.grib2 (the same bytes) with an .idx that gives forecast steps for all four
messages. For 3 and 4 the extension must still return NULL: the step in the inventory cannot be trusted either when the
reference time is not a run time.

Requires: pip install eccodes
Run from the repository root.
"""

import eccodes as ec

SRC = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
OUT = "test/data/reference_significance.grib2"
EXPECTED = "test/data/reference_significance_expected.tsv"
source = open(SRC, "rb").read()

cases = [  # (significance, forecast time in hours, expected valid time or None)
    (1, 6, "2026-06-02 06:00"),
    (0, 0, "2026-06-02 00:00"),
    (2, 6, None),
    (3, 0, None),
]

with open(OUT, "wb") as out, open(EXPECTED, "w") as tsv:
    tsv.write("message_number\tsignificance\tvalid_time_utc\n")
    for number, (significance, hours, valid) in enumerate(cases, start=1):
        h = ec.codes_new_from_message(source)
        ec.codes_set(h, "significanceOfReferenceTime", significance)
        ec.codes_set(h, "forecastTime", hours)
        ec.codes_write(h, out)
        tsv.write("%d\t%d\t%s\n" % (number, significance, valid or "NULL"))

# The same four messages with an inventory that gives a step for each
data = open(OUT, "rb").read()
length = len(data) // len(cases)
assert length * len(cases) == len(data), "the four messages are expected to have the same length"
open("test/data/reference_significance_with_idx.grib2", "wb").write(data)
steps = ["6 hour fcst", "anl", "6 hour fcst", "anl"]
with open("test/data/reference_significance_with_idx.grib2.idx", "w") as idx:
    for number, step in enumerate(steps, start=1):
        idx.write("%d:%d:d=2026060200:CSNOW:surface:%s:\n" % (number, (number - 1) * length, step))

with open(OUT, "rb") as check:
    for number, (significance, _, _) in enumerate(cases, start=1):
        h = ec.codes_grib_new_from_file(check)
        got = ec.codes_get(h, "significanceOfReferenceTime")
        print("%d  significance %d %s" % (number, got, "ok" if got == significance else "MISMATCH"))
