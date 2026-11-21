"""Writes test/data/valid_times.grib2 and test/data/valid_times_expected.tsv: eight messages whose valid time must come
from the message, not from an .idx.

Each message is the tiny HRRR fixture (a constant field on the 1799 x 1059 Lambert grid) re-encoded with a different
reference time, forecast time or product template. The expected valid times are computed here with Python's datetime,
independently of ecCodes, so the check covers ecCodes' own arithmetic as well as the extension's.

  1  instantaneous, forecast time 90 minutes      a step that is not a whole number of hours
  2  instantaneous, forecast time 1 day           a unit other than hours
  3  instantaneous, forecast time 2 x 3 hours      another unit
  4  instantaneous, forecast hour 6                the ordinary case
  5  interval 0-6 hours (template 4.8)            valid at the end of the interval
  6  interval 0-1 day (template 4.8)              a unit the .idx text parser rejects
  7  reference 2028-02-27 23:00 + 26 hours        crosses a leap day
  8  reference 2026-12-31 23:30 + 1 hour          crosses a year boundary

Requires: pip install eccodes
Run from the repository root.
"""

import calendar
from datetime import datetime, timedelta

import eccodes as ec

SRC = "fixtures/hrrr_msg90_csnow_simple_const.grib2"
OUT = "test/data/valid_times.grib2"
EXPECTED = "test/data/valid_times_expected.tsv"

source = open(SRC, "rb").read()
REF = datetime(2026, 6, 2, 0, 0)


def message(reference=REF, unit=1, step=0, interval_hours=None):
    h = ec.codes_new_from_message(source)
    ec.codes_set(h, "dataDate", int(reference.strftime("%Y%m%d")))
    ec.codes_set(h, "dataTime", int(reference.strftime("%H%M")))
    if interval_hours is None:
        ec.codes_set(h, "stepUnits", unit)
        ec.codes_set(h, "step", step) if unit != 1 else ec.codes_set(h, "forecastTime", step)
        valid = (
            reference
            + {
                0: timedelta(minutes=step),
                1: timedelta(hours=step),
                2: timedelta(days=step),
                10: timedelta(hours=3 * step),
            }[unit]
        )
    else:
        ec.codes_set(h, "productDefinitionTemplateNumber", 8)
        ec.codes_set(h, "indicatorOfUnitOfTimeRange", 1)
        ec.codes_set(h, "forecastTime", 0)
        valid = reference + timedelta(hours=interval_hours)
        ec.codes_set(h, "yearOfEndOfOverallTimeInterval", valid.year)
        ec.codes_set(h, "monthOfEndOfOverallTimeInterval", valid.month)
        ec.codes_set(h, "dayOfEndOfOverallTimeInterval", valid.day)
        ec.codes_set(h, "hourOfEndOfOverallTimeInterval", valid.hour)
        ec.codes_set(h, "minuteOfEndOfOverallTimeInterval", valid.minute)
        ec.codes_set(h, "secondOfEndOfOverallTimeInterval", 0)
        ec.codes_set(h, "indicatorOfUnitForTimeRange", 1)
        ec.codes_set(h, "lengthOfTimeRange", interval_hours)
    return h, valid


cases = [
    message(unit=0, step=90),
    message(unit=2, step=1),
    message(unit=10, step=2),
    message(unit=1, step=6),
    message(interval_hours=6),
    message(interval_hours=24),
    message(reference=datetime(2028, 2, 27, 23, 0), unit=1, step=26),
    message(reference=datetime(2026, 12, 31, 23, 30), unit=1, step=1),
]

with open(OUT, "wb") as out, open(EXPECTED, "w") as tsv:
    tsv.write("message_number\tvalid_time_utc\tepoch_microseconds\n")
    for number, (h, valid) in enumerate(cases, start=1):
        ec.codes_write(h, out)
        tsv.write(
            "%d\t%s\t%d\n" % (number, valid.strftime("%Y-%m-%d %H:%M"), calendar.timegm(valid.timetuple()) * 1000000)
        )

# Report what ecCodes itself derives, so a regenerated file that disagrees with the expectation is noticed
with open(OUT, "rb") as check:
    for number, (_, valid) in enumerate(cases, start=1):
        h = ec.codes_grib_new_from_file(check)
        got = "%08d %04d" % (ec.codes_get(h, "validityDate"), ec.codes_get(h, "validityTime"))
        want = valid.strftime("%Y%m%d %H%M")
        print("%d  ecCodes %s  expected %s  %s" % (number, got, want, "ok" if got == want else "MISMATCH"))
