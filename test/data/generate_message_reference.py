"""Regenerate the reference data for test/cpp/idx_check.cpp.

Writes:
  test/data/nbm_idx_expected.tsv    expected parse of fixtures/blend.t00z.core.f001.co.grib2.idx (independent regex
                                    implementation, not the C++ parser)
  test/data/multi.grib2             four messages on three different grids: Guam, Puerto Rico, Hawaii and sample.grib2
  test/data/multi.grib2.idx         SYNTHETIC wgrib2-style inventory for multi.grib2, built from ecCodes keys. It is a
                                    test input for the sidecar code path, not output of wgrib2.
  test/data/sections_reference.tsv  section-header facts for every fixture message, read with ecCodes
  test/data/packing_templates.tsv   data representation template number -> ecCodes packingType, from encoding samples
  test/data/stale.grib2(.idx)       two copies of sample.grib2 with an .idx whose offsets do not match the file, for the
                                    "does this .idx belong to the file?" check

Requires: pip install eccodes ecmwflibs
Run from the repository root.
"""

import re
import struct

import eccodes as ec

NBM_IDX = "fixtures/blend.t00z.core.f001.co.grib2.idx"
# Size of blend.t00z.core.f001.co.grib2 in the NBM bucket listing; the .idx cannot say how long the last message is
NBM_FILE_SIZE = 156464859

MULTI_PARTS = [
    "fixtures/nbm_gu_msg.grib2",
    "fixtures/nbm_pr_msg.grib2",
    "fixtures/nbm_hi_msg.grib2",
    "test/data/sample.grib2",
]
SECTION_FILES = ["fixtures/nbm_msg.grib2", "fixtures/nbm_ak_msg.grib2"] + MULTI_PARTS


def num(value):
    return "NA" if value is None else format(value, ".12g")


def write_nbm_expected():
    line_re = re.compile(r"^(\d+):(\d+):d=(\d{4})(\d{2})(\d{2})(\d{2}):([^:]*):([^:]*):([^:]*)(?::(.*))?$")
    rows = []
    for raw in open(NBM_IDX).read().splitlines():
        m = line_re.match(raw)
        assert m, raw
        msg, off, yy, mo, dd, hh, var, lvl, fc, qual = m.groups()
        qual = qual or ""
        step = 0 if fc == "anl" else None
        sm = re.fullmatch(r"(\d+) hour fcst", fc)
        if sm:
            step = int(sm.group(1))
        pct = None
        pm = re.fullmatch(r"([\d.]+)% level", qual)
        if pm:
            pct = float(pm.group(1))
        lower = upper = None
        if qual.startswith("prob "):
            expr = qual[5:].split(":")[0]
            for op, val in re.findall(r"([<>]=?)([-+\d.eE]+)", expr):
                if op.startswith(">"):
                    lower = float(val)
                else:
                    upper = float(val)
        rows.append(
            [int(msg), int(off), "%s-%s-%sT%s:00:00" % (yy, mo, dd, hh), var, lvl, fc, qual, step, pct, lower, upper]
        )
    with open("test/data/nbm_idx_expected.tsv", "w") as out:
        out.write(
            "# message\toffset\tlength\treference_time\tvariable\tlevel\tforecast\tqualifier\tstep_hours\tpercentile\tlower\tupper\n"
        )
        for i, r in enumerate(rows):
            length = rows[i + 1][1] - r[1] if i + 1 < len(rows) else NBM_FILE_SIZE - r[1]
            out.write(
                "\t".join(
                    [
                        str(r[0]),
                        str(r[1]),
                        str(length),
                        r[2],
                        r[3],
                        r[4],
                        r[5],
                        r[6],
                        num(r[7]),
                        num(r[8]),
                        num(r[9]),
                        num(r[10]),
                    ]
                )
                + "\n"
            )
    print("nbm_idx_expected.tsv:", len(rows), "messages")


def message_boundaries(data):
    spans, pos = [], 0
    while pos < len(data):
        assert data[pos : pos + 4] == b"GRIB"
        length = struct.unpack(">Q", data[pos + 8 : pos + 16])[0]
        spans.append((pos, length))
        pos += length
    return spans


def write_multi():
    data = b"".join(open(p, "rb").read() for p in MULTI_PARTS)
    open("test/data/multi.grib2", "wb").write(data)
    lines = []
    for number, (offset, length) in enumerate(message_boundaries(data), start=1):
        h = ec.codes_new_from_message(data[offset : offset + length])
        g = lambda k: ec.codes_get(h, k)
        level = "surface" if g("typeOfLevel") == "surface" else "%d m above ground" % g("level")
        step = int(g("step"))
        forecast = "anl" if step == 0 else "%d hour fcst" % step
        date = "%08d%02d" % (g("dataDate"), g("dataTime") // 100)
        lines.append("%d:%d:d=%s:%s:%s:%s:" % (number, offset, date, g("shortName").upper(), level, forecast))
    open("test/data/multi.grib2.idx", "w").write("\n".join(lines) + "\n")
    print("multi.grib2:", len(data), "bytes,", len(lines), "messages")


def write_sections():
    with open("test/data/sections_reference.tsv", "w") as out:
        out.write(
            "# file\tdiscipline\tlength\tyear\tmonth\tday\thour\tminute\tgrid_template\tgrid_points\tproduct_template\tcategory\tnumber\tdata_template\tdata_points\tpacking_type\n"
        )
        for path in SECTION_FILES:
            data = open(path, "rb").read()
            h = ec.codes_new_from_message(data)
            g = lambda k: ec.codes_get(h, k)
            date, time = g("dataDate"), g("dataTime")
            out.write(
                "\t".join(
                    str(v)
                    for v in [
                        path,
                        g("discipline"),
                        len(data),
                        date // 10000,
                        date // 100 % 100,
                        date % 100,
                        time // 100,
                        time % 100,
                        g("gridDefinitionTemplateNumber"),
                        g("numberOfDataPoints"),
                        g("productDefinitionTemplateNumber"),
                        g("parameterCategory"),
                        g("parameterNumber"),
                        g("dataRepresentationTemplateNumber"),
                        g("numberOfValues"),
                        g("packingType"),
                    ]
                )
                + "\n"
            )
    print("sections_reference.tsv:", len(SECTION_FILES), "files")


def write_packing_templates():
    names = [
        "grid_simple",
        "grid_complex",
        "grid_complex_spatial_differencing",
        "grid_ieee",
        "grid_jpeg",
        "grid_png",
        "grid_ccsds",
        "grid_run_length",
        "grid_simple_log_preprocessing",
    ]
    rows = []
    for name in names:
        h = ec.codes_grib_new_from_samples("regular_ll_sfc_grib2")
        ec.codes_set(h, "packingType", name)
        template = ec.codes_get(h, "dataRepresentationTemplateNumber")
        assert ec.codes_get(h, "packingType") == name
        rows.append((template, name))
    with open("test/data/packing_templates.tsv", "w") as out:
        out.write("# data_template\tpacking_type\n")
        for template, name in rows:
            out.write("%d\t%s\n" % (template, name))
    print("packing_templates.tsv:", len(rows), "templates")


def write_stale():
    sample = open("test/data/sample.grib2", "rb").read()
    open("test/data/stale.grib2", "wb").write(sample + sample)
    # The second message really starts at byte 277, not 100
    open("test/data/stale.grib2.idx", "w").write("1:0:d=2026010100:T:surface:anl:\n2:100:d=2026010100:T:surface:anl:\n")
    print("stale.grib2:", 2 * len(sample), "bytes, idx offsets deliberately wrong")


write_nbm_expected()
write_multi()
write_sections()
write_packing_templates()
write_stale()
