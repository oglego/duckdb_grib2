"""Regenerate test/data/grid_reference.txt, the oracle for test/cpp/grid_check.cpp.

Everything in the output comes from ecCodes (latitudes/longitudes arrays, the geography iterator and brute-force
searches over them), never from the code under test.

Requires: pip install eccodes ecmwflibs numpy
Run from the repository root.
"""

import numpy as np
import eccodes as ec

SRC = "fixtures/nbm_msg.grib2"
OUT = "test/data/grid_reference.txt"

h = ec.codes_new_from_message(open(SRC, "rb").read())
g = lambda k: ec.codes_get(h, k)
ni, nj = g("Ni"), g("Nj")
assert g("iScansNegatively") == 0 and g("jScansPositively") == 1 and g("jPointsAreConsecutive") == 0
alt = int(g("alternativeRowScanning"))

lat = np.asarray(ec.codes_get_double_array(h, "latitudes")).reshape(nj, ni)
lon = np.asarray(ec.codes_get_double_array(h, "longitudes")).reshape(nj, ni)
lonw = (lon + 180.0) % 360.0 - 180.0
raw = np.asarray(ec.codes_get_double_array(h, "values")).reshape(nj, ni)

# Oracle for value order: ecCodes' own geography iterator yields aligned (lat, lon, value) in canonical order.
it = ec.codes_grib_iterator_new(h, 0)
oracle = np.array([ec.codes_grib_iterator_next(it)[2] for _ in range(4 * ni)]).reshape(4, ni)
ec.codes_grib_iterator_delete(it)

rng = np.random.default_rng(42)
lines = []
lines.append(
    "grid %d %d %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %.17g %d"
    % (
        ni,
        nj,
        g("latitudeOfFirstGridPointInDegrees"),
        g("longitudeOfFirstGridPointInDegrees"),
        g("LaDInDegrees"),
        g("LoVInDegrees"),
        g("Latin1InDegrees"),
        g("Latin2InDegrees"),
        g("DxInMetres"),
        g("DyInMetres"),
        g("radius"),
        alt,
    )
)

# Sample cells with an unambiguous raw index: the cell value must differ from its mirror in the row
n = 0
while n < 150:
    j, i = int(rng.integers(0, nj)), int(rng.integers(0, ni))
    if raw[j, i] == raw[j, ni - 1 - i]:
        continue
    expected_raw = j * ni + ((ni - 1 - i) if (alt and j % 2) else i)
    flat = raw.ravel()
    # the value ecCodes assigns to this (lat, lon) in canonical order is raw at the mirrored position on odd rows
    lines.append("cell %d %d %.17g %.17g %d" % (i, j, lat[j, i], lonw[j, i], expected_raw))
    n += 1

# Check the oracle claim for the first four rows
for jj in range(4):
    idx = [jj * ni + ((ni - 1 - i) if (alt and jj % 2) else i) for i in range(ni)]
    assert np.allclose(raw.ravel()[idx], oracle[jj]), "values ordering assumption failed"


# Query points: clearly inside (brute-force nearest within 2 km) or clearly outside (more than 50 km away)
def brute_nearest(qlat, qlon):
    p1, p2 = np.radians(qlat), np.radians(lat)
    dl = np.radians(lonw - qlon)
    a = np.sin((p2 - p1) / 2) ** 2 + np.cos(p1) * np.cos(p2) * np.sin(dl / 2) ** 2
    d = 2 * 6371.2 * np.arcsin(np.sqrt(a))
    k = int(np.argmin(d))
    return k, float(d.flat[k])


points = 0
candidates = [(37.2090, -93.2923), (10.0, -100.0), (60.0, -100.0), (19.3, -125.0)]
candidates += [(float(rng.uniform(21, 54)), float(rng.uniform(-128, -66))) for _ in range(80)]
for qlat, qlon in candidates:
    k, d = brute_nearest(qlat, qlon)
    j, i = divmod(k, ni)
    if d < 2.0:
        lines.append("point %.17g %.17g %d %d %.17g" % (qlat, qlon, i, j, d))
        points += 1
    elif d > 50.0:
        lines.append("point %.17g %.17g -1 -1 0" % (qlat, qlon))
        points += 1

boxes = [
    (-95, 36, -92, 38),
    (-125, 24, -66, 50),
    (-100.5, 30.2, -99.9, 30.8),
    (-80, 40, -70, 47),
    (-140, 15, -100, 35),
    (-130, 45, -120, 60),
    (-60, 20, -50, 30),
    (-180, 0, 180, 90),
    (170, 50, -170, 60),
    (-180, -90, 180, 90),
    (260, 30, 270, 40),
]
for _ in range(12):
    la0, lo0 = rng.uniform(20, 55), rng.uniform(-125, -68)
    boxes.append((float(lo0), float(la0), float(lo0 + rng.uniform(0.1, 12)), float(la0 + rng.uniform(0.1, 8))))
for lo_min, la_min, lo_max, la_max in boxes:
    span = lo_max - lo_min
    if span < 0:
        span += 360.0
    span = min(span, 360.0)
    east = (lonw - lo_min) % 360.0  # degrees east of the box's western edge, in [0, 360)
    count = int(np.count_nonzero((lat >= la_min) & (lat <= la_max) & (east <= span)))
    lines.append("box %.17g %.17g %.17g %.17g %d" % (lo_min, la_min, lo_max, la_max, count))

open(OUT, "w").write("\n".join(lines) + "\n")
print("wrote", OUT, "cells=150 points=%d boxes=%d" % (points, len(boxes)))
