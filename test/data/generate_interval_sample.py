"""Writes test/data/interval_steps.grib2 and its .idx: four small messages with interval forecast fields.

The messages are the tiny HRRR fixtures (a constant field and a spatially differenced one) repeated, so the file is
a few hundred bytes. The inventory is SYNTHETIC: it labels the messages the way wgrib2 labels accumulations, maxima
and an unknown statistic, to test how valid_time and step_hours are derived from the forecast text. It is a test input
for the sidecar code path, not output of wgrib2, and the labels do not describe the data. The reference time in each
line is the one in the message itself, because the extension refuses an inventory whose reference time disagrees with
the message.

  1  "0-6 hour acc fcst"        valid at the end of the interval: reference time + 6 hours
  2  "6-12 hour max fcst"       reference time + 12 hours
  3  "1-2 hour missing fcst"    reference time + 2 hours (a statistic wgrib2 cannot name still has an interval)
  4  "15 min fcst"              not a whole number of hours: no step, so valid_time is NULL

Pure Python; run from the repository root: python3 test/data/generate_interval_sample.py
"""

PARTS = [
    "fixtures/hrrr_msg90_csnow_simple_const.grib2",
    "fixtures/hrrr_msg166_icec_spatial.grib2",
    "fixtures/hrrr_msg90_csnow_simple_const.grib2",
    "fixtures/hrrr_msg166_icec_spatial.grib2",
]
LABELS = [
    ("APCP", "surface", "0-6 hour acc fcst"),
    ("TMP", "2 m above ground", "6-12 hour max fcst"),
    ("THUNC", "entire atmosphere", "1-2 hour missing fcst"),
    ("APCP", "surface", "15 min fcst"),
]


def reference_time(message):
    """YYYYMMDDHH from section 1 (octets 13-19 of the section: year, month, day, hour, minute, second)."""
    section1 = message[16:]
    assert section1[4] == 1, "section 1 must follow the indicator section"
    year = int.from_bytes(section1[12:14], "big")
    month, day, hour, minute, second = section1[14:19]
    assert minute == 0 and second == 0, "the .idx records the hour only"
    return "%04d%02d%02d%02d" % (year, month, day, hour)


data = b""
lines = []
for number, (path, (variable, level, forecast)) in enumerate(zip(PARTS, LABELS), start=1):
    message = open(path, "rb").read()
    assert message[:4] == b"GRIB" and message[-4:] == b"7777", path
    lines.append("%d:%d:d=%s:%s:%s:%s:" % (number, len(data), reference_time(message), variable, level, forecast))
    data += message

open("test/data/interval_steps.grib2", "wb").write(data)
open("test/data/interval_steps.grib2.idx", "w").write("\n".join(lines) + "\n")
print("interval_steps.grib2:", len(data), "bytes,", len(PARTS), "messages")
