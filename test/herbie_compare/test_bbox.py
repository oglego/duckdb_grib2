"""read_grib2(bbox := [lon_min, lat_min, lon_max, lat_max]) against Herbie's lat/lon fields."""

from __future__ import annotations

import numpy as np
import pytest

from checks import check_bbox, check_points
from conftest import GRID_NAMES, SMALL_GRIDS
from reference import wrap180

BIG = {"co", "co_complex", "ak"}


def _box_around(ref, i, j, half_cells):
    """A lon/lat box centred on cell (i, j), ``half_cells`` cell spacings in each direction (in degrees, locally)."""
    i2, j2 = min(i + 1, ref.ni - 1), min(j + 1, ref.nj - 1)
    i1, j1 = max(i - 1, 0), max(j - 1, 0)
    dlat = max(abs(ref.lat[j2, i] - ref.lat[j1, i]), abs(ref.lat[j, i2] - ref.lat[j, i1])) / 2
    dlon = max(abs(wrap180(ref.lon[j, i2] - ref.lon[j, i1])), abs(wrap180(ref.lon[j2, i] - ref.lon[j1, i]))) / 2
    lat, lon = float(ref.lat[j, i]), float(ref.lon[j, i])
    return [
        lon - half_cells * dlon,
        max(lat - half_cells * dlat, -90.0),
        lon + half_cells * dlon,
        min(lat + half_cells * dlat, 90.0),
    ]


# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("name", GRID_NAMES)
@pytest.mark.parametrize("half_cells", [0.3, 1.0, 2.5, 8.0, 25.0])
def test_boxes_of_many_sizes(name, half_cells, grid, rg):
    ref, path = grid(name)
    rng = np.random.default_rng(int(half_cells * 100))
    n_boxes = 3 if name in BIG else 5
    for k in range(n_boxes):
        i = int(rng.integers(5, ref.ni - 5))
        j = int(rng.integers(5, ref.nj - 5))
        bbox = _box_around(ref, i, j, half_cells)
        check_bbox(rg, ref, bbox, path=path, context=f"{name} box {k} half={half_cells} at ({i},{j})")


@pytest.mark.parametrize("name", GRID_NAMES)
def test_box_that_falls_between_cell_centres_is_empty_or_consistent(name, grid, rg):
    ref, path = grid(name)
    i, j = ref.ni // 2, ref.nj // 2
    la = (ref.lat[j, i] + ref.lat[j + 1, i + 1]) / 2
    lo = ref.lon[j, i] + wrap180(ref.lon[j + 1, i + 1] - ref.lon[j, i]) / 2
    tiny = [lo - 1e-4, la - 1e-4, lo + 1e-4, la + 1e-4]
    df = check_bbox(rg, ref, tiny, path=path, context=f"{name} tiny box")
    assert len(df) == 0, "a box much smaller than the cell spacing, centred between centres, holds no cell centre"


@pytest.mark.parametrize("name", GRID_NAMES)
def test_box_far_from_the_grid_is_empty(name, grid, rg):
    ref, path = grid(name)
    lo = float(wrap180(ref.lon.mean() + 180.0))  # the opposite side of the world
    la = -float(np.sign(ref.lat.mean())) * 40.0
    df = check_bbox(rg, ref, [lo - 5, la - 5, lo + 5, la + 5], path=path, context=name)
    assert len(df) == 0


@pytest.mark.parametrize("name", GRID_NAMES)
def test_box_partly_outside_the_grid(name, grid, rg):
    """A box that starts inside the grid and extends well beyond its corner returns only the overlapping cells."""
    ref, path = grid(name)
    la, lo = float(ref.lat[ref.nj - 1, ref.ni - 1]), float(ref.lon[ref.nj - 1, ref.ni - 1])
    c_la, c_lo = float(ref.lat[ref.nj // 2, ref.ni // 2]), float(ref.lon[ref.nj // 2, ref.ni // 2])
    lon_lo, lon_hi = (min(lo, c_lo), max(lo, c_lo))
    if lon_hi - lon_lo > 180:  # straddles the seam: use a safer box
        lon_lo, lon_hi = c_lo - 1.0, c_lo + 1.0
    bbox = [lon_lo - 0.5, min(la, c_la) - 0.2, lon_hi + 3.0, max(la, c_la) + 3.0]
    bbox[3] = min(bbox[3], 90.0)
    check_bbox(rg, ref, bbox, path=path, context=name)


@pytest.mark.parametrize("name", SMALL_GRIDS)
@pytest.mark.parametrize("box", [[-180, -90, 180, 90], [0, -90, 360, 90], [-360, -90, 0, 90], [-180, -90, 0, 90]])
def test_world_boxes_small_grids(name, box, grid, rg):
    """Whole-world boxes (either longitude convention, or a half world) select the expected cells."""
    ref, path = grid(name)
    df = check_bbox(rg, ref, box, path=path, context=f"{name} {box}")
    if box[2] - box[0] >= 360:
        assert len(df) == ref.n_cells


@pytest.mark.slow
@pytest.mark.parametrize("name", sorted(BIG))
def test_world_box_big_grids(name, grid, rg):
    ref, path = grid(name)
    df = check_bbox(rg, ref, [-180, -90, 180, 90], path=path, context=name)
    assert len(df) == ref.n_cells


# ---------------------------------------------------------------------------------------------------------------------
# Longitude conventions and the antimeridian
# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("name", GRID_NAMES)
def test_same_box_in_both_longitude_conventions(name, grid, rg):
    ref, path = grid(name)
    # 6.37 cells, not a whole number: on Mercator grids a whole number puts the edges exactly on cell-centre
    # longitudes, where the three spellings below round differently by one ulp and may disagree about the edge column
    bbox = _box_around(ref, ref.ni // 2, ref.nj // 2, 6.37)
    a = check_bbox(rg, ref, bbox, path=path, context=f"{name} -180..180")
    shifted = [bbox[0] % 360.0, bbox[1], bbox[2] % 360.0, bbox[3]]
    b = check_bbox(rg, ref, shifted, path=path, context=f"{name} 0..360")
    c = check_bbox(rg, ref, [bbox[0] - 360.0, bbox[1], bbox[2] - 360.0, bbox[3]], path=path,
                   context=f"{name} -540..-180")  # fmt: skip
    assert len(a) == len(b) == len(c) > 0
    assert (a[["i", "j"]].to_numpy() == b[["i", "j"]].to_numpy()).all()
    assert (a[["i", "j"]].to_numpy() == c[["i", "j"]].to_numpy()).all()


@pytest.mark.parametrize(
    "lon_min,lon_max",
    [(170.0, -170.0), (170.0, 190.0), (-190.0, -170.0), (175.0, -175.0), (179.5, -179.5), (-180.0, -179.0), (179.0, 180.0),
     (180.0, -180.0 + 5.0), (-175.0, 175.0)],
)  # fmt: skip
def test_alaska_antimeridian_boxes(lon_min, lon_max, grid, rg):
    """lon_max < lon_min means the box wraps through 180 degrees; the same box in 0..360 spelling must agree."""
    ref, path = grid("ak")
    bbox = [lon_min, 45.0, lon_max, 75.0]
    df = check_bbox(rg, ref, bbox, path=path, context=f"ak {bbox}")
    if (lon_min, lon_max) in ((170.0, -170.0), (175.0, -175.0)):
        assert len(df) > 0, "the Aleutians lie on both sides of the seam"
        again = check_bbox(rg, ref, [lon_min % 360, 45.0, lon_max % 360, 75.0], path=path, context="ak 0..360 spelling")
        assert len(again) == len(df)


def test_alaska_polar_and_high_latitude_boxes(grid, rg):
    """Polar stereographic: a lat/lon rectangle is a curved wedge on the grid; includes boxes touching the pole."""
    ref, path = grid("ak")
    boxes = [
        [-180, 80, 180, 90],  # polar cap
        [-180, 60, 180, 90],  # everything north of 60
        [-150, 60, -140, 70],  # ordinary mid-Alaska box
        [-165, 62, -140, 72],
        [-125, 54, -130, 60],  # lon_max < lon_min wraps almost all the way round the world
        [-180, 40, 180, 55],  # southern band
        [0, 60, 360, 90],
        [-100, 85, -80, 90],  # tiny wedge at the pole
    ]
    for b in boxes:
        check_bbox(rg, ref, b, path=path, context=f"ak {b}")


@pytest.mark.parametrize("name", GRID_NAMES)
def test_latitude_limits_accepted(name, grid, rg):
    ref, path = grid(name)
    lon = float(ref.lon[ref.nj // 2, ref.ni // 2])
    check_bbox(rg, ref, [lon - 1, -90, lon + 1, 90], path=path, context=f"{name} full-latitude strip")
    check_bbox(rg, ref, [lon - 1, 90, lon + 1, 90], path=path, context=f"{name} degenerate at the pole")


# ---------------------------------------------------------------------------------------------------------------------
# Exact boundaries, consistency with points, masks
# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("name", GRID_NAMES)
def test_degenerate_box_on_a_cell_centre_returns_that_cell(name, grid, rg):
    """Edges are inclusive: a zero-size box placed exactly on a cell centre (as the extension itself reports it)."""
    ref, path = grid(name)
    i, j = ref.ni // 3, ref.nj // 3
    pt = rg.read(path, points=[(float(ref.lat[j, i]), float(ref.lon[j, i]))])
    assert (int(pt["i"][0]), int(pt["j"][0])) == (i, j)
    lat, lon = float(pt["lat"][0]), float(pt["lon"][0])
    df = rg.read(path, bbox=[lon, lat, lon, lat])
    assert [(int(a), int(b)) for a, b in zip(df["i"], df["j"])] == [
        (i, j)
    ], "a box whose edges pass through a cell centre contains it"


@pytest.mark.parametrize("name", ["hi", "pr", "gu"])
def test_exact_edges_are_inclusive_on_mercator_grids(name, grid, rg):
    """On Mercator grids latitude is constant along rows and longitude along columns, so a box whose edges are exactly
    the lat/lon of a block of cells must return that whole block, boundary rows and columns included."""
    ref, path = grid(name)
    i0, i1, j0, j1 = ref.ni // 4, ref.ni // 4 + 6, ref.nj // 4, ref.nj // 4 + 4
    probe = rg.read(path, points=[(float(ref.lat[j, i]), float(ref.lon[j, i])) for i, j in ((i0, j0), (i1, j1))])
    lon_min, lat_min = float(probe["lon"][0]), float(probe["lat"][0])
    lon_max, lat_max = float(probe["lon"][1]), float(probe["lat"][1])
    df = rg.read(path, bbox=[lon_min, lat_min, lon_max, lat_max])
    got = set(zip(df["i"].astype(int), df["j"].astype(int)))
    want = {(i, j) for i in range(i0, i1 + 1) for j in range(j0, j1 + 1)}
    assert got == want, f"missing {sorted(want - got)[:5]}, extra {sorted(got - want)[:5]}"


@pytest.mark.parametrize("name", ["hi", "co"])
def test_bbox_rows_agree_with_points_queries(name, grid, rg):
    """Cross-check inside the extension: every cell a bbox returns, queried as a point, resolves to itself."""
    ref, path = grid(name)
    bbox = _box_around(ref, ref.ni // 2, ref.nj // 2, 3.0)
    box = check_bbox(rg, ref, bbox, path=path, context=name)
    pts = list(zip(box["lat"].astype(float), box["lon"].astype(float)))
    pt = check_points(rg, ref, pts, path=path, context=f"{name} bbox cells as points")
    assert (pt[["i", "j"]].to_numpy() == box[["i", "j"]].to_numpy()).all()
    np.testing.assert_allclose(pt["value"].to_numpy(dtype=float), box["value"].to_numpy(dtype=float), equal_nan=True)


def test_masked_cells_inside_a_box_are_null_not_dropped(grid, rg):
    ref, path = grid("gu_masked")
    masked = np.argwhere(np.isnan(ref.values))
    j, i = masked[len(masked) // 2]
    bbox = _box_around(ref, int(i), int(j), 12.0)
    df = check_bbox(rg, ref, bbox, path=path, context="gu_masked")
    expected_nulls = int(np.isnan(ref.values[ref.box_mask(*bbox)]).sum())
    assert expected_nulls > 0, "test box should contain masked cells"
    assert int(df["value"].isna().sum()) == expected_nulls
    # the box used by the extension's own test-suite
    check_bbox(rg, ref, [145.092, 14.646, 145.607, 15.145], path=path, context="gu_masked sql-test box")


@pytest.mark.parametrize("name", GRID_NAMES)
def test_bbox_rows_are_in_ascending_grid_order(name, grid, rg):
    ref, path = grid(name)
    df = rg.read(path, bbox=_box_around(ref, ref.ni // 2, ref.nj // 2, 10.0))
    flat = df["j"].to_numpy(dtype=np.int64) * ref.ni + df["i"].to_numpy(dtype=np.int64)
    assert len(flat) > 0 and (np.diff(flat) > 0).all()
