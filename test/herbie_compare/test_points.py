"""read_grib2(points := ...) against Herbie: nearest cell, value, distance, ordering and the edge cases around them."""

from __future__ import annotations

import numpy as np
import pytest

from checks import check_points, jittered_points, lon_diff, to_360
from conftest import GRID_NAMES, SMALL_GRIDS
from reference import wrap180


def _centres(ref, cells):
    return [(float(ref.lat[j, i]), float(ref.lon[j, i])) for i, j in cells]


def _outward(ref, i, j, scale):
    """A point ``scale`` cell spacings beyond the centre of edge/corner cell (i, j), in the outward direction."""
    di = -1 if i == 0 else (1 if i == ref.ni - 1 else 0)
    dj = -1 if j == 0 else (1 if j == ref.nj - 1 else 0)
    assert di or dj, "not an edge cell"
    dlat = dlon = 0.0
    for a, b in ((di, 0), (0, dj)):
        if a or b:
            ni_, nj_ = i - a, j - b  # inner neighbour
            dlat += ref.lat[j, i] - ref.lat[nj_, ni_]
            dlon += float(wrap180(ref.lon[j, i] - ref.lon[nj_, ni_]))
    return float(ref.lat[j, i] + scale * dlat), float(wrap180(ref.lon[j, i] + scale * dlon))


# ---------------------------------------------------------------------------------------------------------------------
# Core correctness
# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("name", GRID_NAMES)
def test_cell_centres_return_exactly_that_cell(name, grid, rg):
    ref, path = grid(name)
    rng = np.random.default_rng(11)
    cells = [(int(rng.integers(0, ref.ni)), int(rng.integers(0, ref.nj))) for _ in range(60)]
    cells += [(0, 0), (ref.ni - 1, 0), (0, ref.nj - 1), (ref.ni - 1, ref.nj - 1)]
    df = check_points(rg, ref, _centres(ref, cells), path=path, context=name)
    assert len(df) == len(cells)
    assert list(zip(df["i"], df["j"])) == cells, "a cell centre must map back to its own (i, j)"
    assert df["distance_km"].max() < 1e-3, "distance to a cell centre should be ~0"


@pytest.mark.parametrize("name", GRID_NAMES)
def test_random_interior_points(name, grid, rg):
    ref, path = grid(name)
    pts = jittered_points(ref, 300, seed=2024)
    df = check_points(rg, ref, pts, path=path, context=name)
    assert len(df) == len(pts), "every interior point must have a row"
    # a point in the interior is never farther than about 0.71 cell spacings from its nearest centre
    assert df["distance_km"].max() <= 0.8 * ref.typical_spacing_km * 1.15


@pytest.mark.parametrize("name", [n for n in GRID_NAMES if n != "co_complex"])
def test_alternating_row_scan_values_not_mirrored(name, grid, rg):
    """The trap documented in docs/DESIGN.md: with alternative row scanning, odd rows are stored east-to-west.

    Picks odd-row cells where the mirrored cell has a *different* value, so reading values in stored order would be
    detected, and checks both row parities are exercised.
    """
    ref, path = grid(name)
    if not ref.alt_row_scanning:
        pytest.skip("grid does not use alternative row scanning")
    rng = np.random.default_rng(99)
    cells = []
    tries = 0
    while len(cells) < 40 and tries < 5000:
        tries += 1
        i, j = int(rng.integers(0, ref.ni)), 2 * int(rng.integers(0, ref.nj // 2)) + 1  # odd row
        v, vm = ref.values[j, i], ref.values[j, ref.ni - 1 - i]
        if np.isnan(v) or np.isnan(vm) or abs(v - vm) < 1e-3 or i == ref.ni - 1 - i:
            continue
        cells.append((i, j))
    assert len(cells) >= 20, "fixture has too few cells that distinguish stored order from canonical order"
    even = [(i, j - 1) for i, j in cells[:20]]
    df = check_points(rg, ref, _centres(ref, cells + even), path=path, context=name)
    assert (df["j"] % 2 == 1).sum() >= 20 and (df["j"] % 2 == 0).sum() >= 20


@pytest.mark.parametrize("name", ["co", "co_complex", "hi"])
def test_equidistant_and_near_tie_points(name, grid, rg):
    """Midpoints between 2 or 4 cell centres: the choice may differ only when the distances genuinely tie."""
    ref, path = grid(name)
    rng = np.random.default_rng(5)
    pts = []
    for _ in range(80):
        i, j = int(rng.integers(2, ref.ni - 3)), int(rng.integers(2, ref.nj - 3))
        for di, dj in ((1, 0), (0, 1), (1, 1)):
            la = (ref.lat[j, i] + ref.lat[j + dj, i + di]) / 2
            lo = ref.lon[j, i] + wrap180(ref.lon[j + dj, i + di] - ref.lon[j, i]) / 2
            pts.append((float(la), float(wrap180(lo))))
    check_points(rg, ref, pts, path=path, context=name)


def test_many_points_bulk(grid, rg):
    ref, path = grid("co")
    pts = jittered_points(ref, 3000, seed=3)
    df = check_points(rg, ref, pts, path=path, context="co bulk")
    assert len(df) == 3000


# ---------------------------------------------------------------------------------------------------------------------
# Edges of the grid and outside it
# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("name", GRID_NAMES)
def test_corners_edges_and_just_beyond(name, grid, rg):
    ref, path = grid(name)
    ni, nj = ref.ni, ref.nj
    edge_cells = [(0, 0), (ni - 1, 0), (0, nj - 1), (ni - 1, nj - 1),
                  (ni // 2, 0), (ni // 2, nj - 1), (0, nj // 2), (ni - 1, nj // 2)]  # fmt: skip
    # 1) edge cell centres
    check_points(rg, ref, _centres(ref, edge_cells), path=path, context=f"{name} edge centres")
    # 2) 0.45 spacings beyond an edge cell centre is still inside the footprint (half-cell rule): row expected
    #    (classification by the reference decides what is certain; 0.45 is certainly inside)
    near = [_outward(ref, i, j, 0.45) for i, j in edge_cells[4:]]
    df = check_points(rg, ref, near, path=path, context=f"{name} just inside the edge")
    assert len(df) == len(near), "points within half a cell of the edge belong to the edge cell"
    # 3) 1.5 spacings beyond is outside: no row
    far = [_outward(ref, i, j, 1.5) for i, j in edge_cells]
    df = check_points(rg, ref, far, path=path, context=f"{name} beyond the edge")
    assert len(df) == 0, "points more than a cell outside the grid must not produce rows"


@pytest.mark.parametrize("name", GRID_NAMES)
def test_points_nowhere_near_the_grid(name, grid, rg):
    ref, path = grid(name)
    centre_lat, centre_lon = float(ref.lat[ref.nj // 2, ref.ni // 2]), float(ref.lon[ref.nj // 2, ref.ni // 2])
    pts = [(0.0, 0.0), (-89.0, 10.0), (89.9, -170.0), (-centre_lat, wrap180(centre_lon + 180.0)), (0.0, 179.9),
           (45.0, 120.0), (-33.0, -70.0)]  # fmt: skip
    pts = [p for p in pts if ref.classify_point(*p)[0] == "outside"]
    df = check_points(rg, ref, pts, path=path, context=name)
    assert len(df) == 0


@pytest.mark.parametrize("name", GRID_NAMES)
def test_random_points_over_the_whole_globe(name, grid, rg):
    """Mostly-outside input: every point is classified by the reference, ambiguous edge cases may go either way."""
    ref, path = grid(name)
    rng = np.random.default_rng(31)
    lat = np.degrees(np.arcsin(rng.uniform(-1, 1, 400)))
    lon = rng.uniform(-180, 180, 400)
    # make sure a healthy share lands in the grid
    inside = jittered_points(ref, 100, seed=8)
    pts = list(zip(lat.tolist(), lon.tolist())) + inside
    rng.shuffle(pts)
    check_points(rg, ref, [tuple(p) for p in pts], path=path, context=name)


# ---------------------------------------------------------------------------------------------------------------------
# Longitude conventions, the antimeridian, poles
# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("name", GRID_NAMES)
def test_longitude_given_as_0_360_equals_minus180_180(name, grid, rg):
    ref, path = grid(name)
    pts = jittered_points(ref, 40, seed=17)
    a = check_points(rg, ref, pts, path=path, context=f"{name} -180..180")
    b = check_points(rg, ref, [(la, to_360(lo)) for la, lo in pts], path=path, context=f"{name} 0..360")
    c = check_points(rg, ref, [(la, lo - 360.0 if lo > 0 else lo + 360.0) for la, lo in pts], path=path,
                     context=f"{name} shifted by 360")  # fmt: skip
    for other in (b, c):
        assert (a["i"].to_numpy() == other["i"].to_numpy()).all() and (a["j"].to_numpy() == other["j"].to_numpy()).all()
        np.testing.assert_allclose(
            a["value"].to_numpy(dtype=float), other["value"].to_numpy(dtype=float), equal_nan=True
        )


def test_alaska_across_the_antimeridian(grid, rg):
    """Alaska's grid reaches the Aleutians west of 180: the same cell must be found as 179.x, -180.x and 180+x."""
    ref, path = grid("ak")
    near_seam = np.argwhere(np.abs(np.abs(ref.lon) - 180.0) < 3.0)
    if len(near_seam) < 10:
        pytest.skip("no cells near the antimeridian in this fixture")
    rng = np.random.default_rng(4)
    pick = near_seam[rng.choice(len(near_seam), size=min(40, len(near_seam)), replace=False)]
    variants = []
    for j, i in pick:
        la, lo = float(ref.lat[j, i]), float(ref.lon[j, i])
        variants += [(la, lo), (la, to_360(lo)), (la, lo - 360.0 if lo > 0 else lo + 360.0)]
    df = check_points(rg, ref, variants, path=path, context="ak antimeridian")
    assert len(df) == len(variants)
    # three spellings of one longitude -> one cell
    cells = list(zip(df["i"], df["j"]))
    assert all(cells[k] == cells[k + 1] == cells[k + 2] for k in range(0, len(cells), 3))
    # exactly on the seam
    la = float(np.median(ref.lat[near_seam[:, 0], near_seam[:, 1]]))
    check_points(rg, ref, [(la, 180.0), (la, -180.0), (la, 179.999999), (la, -179.999999), (la, 540.0 - 360.0)], path=path,
                 context="ak on the seam")  # fmt: skip


@pytest.mark.parametrize("name", GRID_NAMES)
def test_extreme_but_valid_inputs_do_not_break(name, grid, rg):
    """lat +/-90 and lon -360, 0, 360 are accepted; the answer must agree with the reference classification."""
    ref, path = grid(name)
    pts = [
        (90.0, 0.0),
        (-90.0, 0.0),
        (90.0, 180.0),
        (0.0, -360.0),
        (0.0, 360.0),
        (0.0, 0.0),
        (90.0, -360.0),
        (-90.0, 360.0),
    ]
    check_points(rg, ref, pts, path=path, context=name)


# ---------------------------------------------------------------------------------------------------------------------
# Result shape: ordering, duplicates, echo of the query, argument types
# ---------------------------------------------------------------------------------------------------------------------
def test_order_duplicates_and_mixed_inside_outside(grid, rg):
    ref, path = grid("co")
    a, b = jittered_points(ref, 2, seed=1)
    out = (0.0, 0.0)
    pts = [a, out, b, a, out, b, b]
    df = check_points(rg, ref, pts, path=path, context="mixed")
    assert len(df) == 5
    assert list(zip(df["query_lat"], df["query_lon"])) == [a, b, a, b, b]
    rev = check_points(rg, ref, pts[::-1], path=path, context="reversed")
    assert list(zip(rev["query_lat"], rev["query_lon"])) == [b, b, a, b, a]


def test_integer_valued_coordinates(grid, rg):
    ref, path = grid("co")
    df = check_points(rg, ref, [(37, -93), (40, -100), (45, -110)], path=path, context="int coords")
    assert len(df) == 3 and list(df["query_lat"]) == [37, 40, 45]


@pytest.mark.parametrize("name", ["gu_masked"])
def test_masked_cells_have_null_value_but_still_a_row(name, grid, rg):
    ref, path = grid(name)
    masked = np.argwhere(np.isnan(ref.values))
    unmasked = np.argwhere(~np.isnan(ref.values))
    rng = np.random.default_rng(2)
    mc = [tuple(int(x) for x in masked[k][::-1]) for k in rng.choice(len(masked), 40, replace=False)]
    uc = [tuple(int(x) for x in unmasked[k][::-1]) for k in rng.choice(len(unmasked), 40, replace=False)]
    df = check_points(rg, ref, _centres(ref, mc + uc), path=path, context=name)
    assert len(df) == 80
    assert df["value"].isna().sum() == 40, "masked cells must give NULL (row kept), unmasked cells a number"
    # points between a masked cell and its neighbours still resolve to the masked cell when it is nearest
    check_points(rg, ref, jittered_points(ref, 300, seed=6), path=path, context=f"{name} jittered")


def test_categorical_values_exact(grid, rg):
    """Template 5.2 present-weather codes (integers 0..76) must come through unchanged."""
    ref, path = grid("co_complex")
    df = check_points(rg, ref, jittered_points(ref, 400, seed=9), path=path, context="co_complex")
    v = df["value"].to_numpy(dtype=float)
    assert (v == np.round(v)).all() and v.min() >= 0 and v.max() <= 76


def test_query_longitude_is_echoed_unchanged(grid, rg):
    ref, path = grid("co")
    la, lo = jittered_points(ref, 1, seed=12)[0]
    df = rg.read(path, points=[(la, to_360(lo)), (la, lo)])
    assert lon_diff(df["query_lon"], lo).max() < 1e-9
    assert abs(df["query_lon"].iloc[0] - to_360(lo)) < 1e-9 and abs(df["query_lon"].iloc[1] - lo) < 1e-9
