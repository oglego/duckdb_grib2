"""Regenerate test/data/grid_reference.txt, the oracle for test/cpp/grid_check.cpp.

Everything in the output comes from ecCodes (latitudes/longitudes arrays, the geography iterator and brute-force
searches over them), never from the code under test. One section per NBM region, each with its own projection.

Requires: pip install eccodes ecmwflibs numpy
Run from the repository root.
"""

import numpy as np
import eccodes as ec

REGIONS = [
    ("co", "fixtures/nbm_msg.grib2"),
    ("ak", "fixtures/nbm_ak_msg.grib2"),
    ("hi", "fixtures/nbm_hi_msg.grib2"),
    ("pr", "fixtures/nbm_pr_msg.grib2"),
    ("gu", "fixtures/nbm_gu_msg.grib2"),
]
OUT = "test/data/grid_reference.txt"
rng = np.random.default_rng(42)
lines = []


def wrap180(x):
    return (x + 180.0) % 360.0 - 180.0


def section(region, path):
    h = ec.codes_new_from_message(open(path, "rb").read())
    g = lambda k: ec.codes_get(h, k)
    ni, nj = g("Ni"), g("Nj")
    grid_type = g("gridType")
    assert g("iScansNegatively") == 0 and g("jScansPositively") == 1 and g("jPointsAreConsecutive") == 0
    assert g("shapeOfTheEarth") == 1, "only the spherical earth with an explicit radius is handled"
    alt = int(g("alternativeRowScanning"))
    radius = g("radius")
    common = (g("latitudeOfFirstGridPointInDegrees"), g("longitudeOfFirstGridPointInDegrees"), g("LaDInDegrees"))
    if grid_type == "lambert":
        params = "%.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g" % (
            *common,
            g("LoVInDegrees"),
            g("Latin1InDegrees"),
            g("Latin2InDegrees"),
            g("DxInMetres"),
            g("DyInMetres"),
            radius,
        )
    elif grid_type == "polar_stereographic":
        assert g("southPoleOnProjectionPlane") == 0
        params = "%.17g %.17g %.17g %.17g %.17g %.17g %.17g" % (
            *common,
            g("orientationOfTheGridInDegrees"),
            g("DxInMetres"),
            g("DyInMetres"),
            radius,
        )
    elif grid_type == "mercator":
        params = "%.17g %.17g %.17g %.17g %.17g %.17g" % (*common, g("DxInMetres"), g("DyInMetres"), radius)
    else:
        raise SystemExit("unsupported grid type " + grid_type)
    lines.append("region " + region)
    lines.append("grid %s %d %d %s %d" % (grid_type, ni, nj, params, alt))

    lat = np.asarray(ec.codes_get_double_array(h, "latitudes")).reshape(nj, ni)
    lon = np.asarray(ec.codes_get_double_array(h, "longitudes")).reshape(nj, ni)
    lonw = wrap180(lon)
    raw = np.asarray(ec.codes_get_double_array(h, "values")).reshape(nj, ni)

    # Oracle for value order: ecCodes' own geography iterator yields aligned (lat, lon, value) in canonical order.
    it = ec.codes_grib_iterator_new(h, 0)
    oracle = np.array([ec.codes_grib_iterator_next(it)[2] for _ in range(4 * ni)]).reshape(4, ni)
    ec.codes_grib_iterator_delete(it)
    for jj in range(4):
        idx = [jj * ni + ((ni - 1 - i) if (alt and jj % 2) else i) for i in range(ni)]
        assert np.allclose(raw.ravel()[idx], oracle[jj]), region + ": values ordering assumption failed"

    # Sample cells with an unambiguous raw index: the cell value must differ from its mirror in the row
    count = 0
    while count < 100:
        j, i = int(rng.integers(0, nj)), int(rng.integers(0, ni))
        if raw[j, i] == raw[j, ni - 1 - i]:
            continue
        expected_raw = j * ni + ((ni - 1 - i) if (alt and j % 2) else i)
        lines.append("cell %d %d %.17g %.17g %d" % (i, j, lat[j, i], lonw[j, i], expected_raw))
        count += 1

    # Query points: clearly inside (within 0.4 cell of a centre) or clearly outside (more than 3 cells away)
    cell_km = g("DxInMetres") / 1000.0

    def brute_nearest(qlat, qlon):
        p1, p2 = np.radians(qlat), np.radians(lat)
        dl = np.radians(lonw - qlon)
        a = np.sin((p2 - p1) / 2) ** 2 + np.cos(p1) * np.cos(p2) * np.sin(dl / 2) ** 2
        d = 2 * (radius / 1000.0) * np.arcsin(np.sqrt(a))
        k = int(np.argmin(d))
        return k, float(d.flat[k])

    lat_lo, lat_hi = float(lat.min()), float(lat.max())
    inside = outside = attempts = 0
    candidates = [(37.2090, -93.2923)] if region == "co" else []
    while attempts < 400 and (inside < 25 or outside < 8):
        attempts += 1
        if candidates:
            qlat, qlon = candidates.pop()
        else:
            qlat = float(rng.uniform(lat_lo - 3, lat_hi + 3))
            qlon = float(rng.uniform(-180, 180))
            if rng.random() < 0.8:  # most candidates near a random cell so many land inside the grid
                j, i = int(rng.integers(0, nj)), int(rng.integers(0, ni))
                qlat = float(lat[j, i] + rng.uniform(-0.05, 0.05))
                qlon = float(lonw[j, i] + rng.uniform(-0.05, 0.05))
        k, d = brute_nearest(qlat, qlon)
        j, i = divmod(k, ni)
        if d < 0.4 * cell_km and inside < 25:
            lines.append("point %.17g %.17g %d %d %.17g" % (qlat, qlon, i, j, d))
            inside += 1
        elif d > 3 * cell_km and outside < 8:
            lines.append("point %.17g %.17g -1 -1 0" % (qlat, qlon))
            outside += 1

    # Boxes: random boxes inside the region, plus whole world, antimeridian style, and one far away.
    # Edges are offset from cell centres by a fraction of a cell: an edge exactly on a centre (or on a whole row of
    # centres, as in Mercator) would make membership depend on 1e-14 rounding noise instead of on the code under test.
    cell_deg = cell_km / 111.2

    def off():
        return float(rng.uniform(0.2, 0.8) * cell_deg)

    boxes = [(-180.0, -90.0, 180.0, 90.0), (170.0, 50.0, -170.0, 60.0)]
    for _ in range(10):
        j, i = int(rng.integers(0, nj)), int(rng.integers(0, ni))
        w, hgt = float(rng.uniform(0.05, 4.0)), float(rng.uniform(0.05, 3.0))
        lo_min, la_min = float(lonw[j, i]) - off(), float(lat[j, i]) - off()
        boxes.append((lo_min, la_min, lo_min + w, la_min + hgt))
    j, i = nj // 2, ni // 2
    boxes.append(
        (
            float(lonw[j, i]) - 1.0 - off(),
            float(lat[j, i]) - 1.0 - off(),
            float(lonw[j, i]) + 1.0,
            float(lat[j, i]) + 1.0,
        )
    )
    far_lon = float(wrap180(lonw[j, i] + 180.0))
    boxes.append((far_lon, -float(lat[j, i]) - 1.0, far_lon + 2.0, -float(lat[j, i]) + 1.0))
    margin = 0.01
    boxes.append((float(lonw.min()) - margin, lat_lo - margin, float(lonw.max()) + margin, lat_hi + margin))
    for lo_min, la_min, lo_max, la_max in boxes:
        span = lo_max - lo_min
        if span < 0:
            span += 360.0
        span = min(span, 360.0)
        east = (lonw - lo_min) % 360.0  # degrees east of the box's western edge, in [0, 360)
        n = int(np.count_nonzero((lat >= la_min) & (lat <= la_max) & (east <= span)))
        lines.append("box %.17g %.17g %.17g %.17g %d" % (lo_min, la_min, lo_max, la_max, n))
    print(
        "%s: %s %dx%d alt=%d cells=%d points=%d+%d outside boxes=%d"
        % (region, grid_type, ni, nj, alt, count, inside, outside, len(boxes))
    )


for region, path in REGIONS:
    section(region, path)

open(OUT, "w").write("\n".join(lines) + "\n")
print("wrote", OUT, "(%d lines)" % len(lines))
