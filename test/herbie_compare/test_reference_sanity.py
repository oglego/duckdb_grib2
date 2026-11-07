"""Does the *reference* deserve trust?  These tests need no extension: they check the Herbie-derived fields against
ecCodes' own geography/nearest-point answers and against brute force, so a failure in the other test modules can be
blamed on the extension rather than on the oracle.
"""

from __future__ import annotations

import eccodes as ec
import numpy as np
import pytest

from conftest import GRID_NAMES
from reference import GRIDS, haversine_km, split_messages, wrap180


@pytest.mark.parametrize("name", GRID_NAMES)
def test_reference_matches_eccodes_nearest_point(name, grid):
    """For random unmasked cells, ecCodes' find_nearest at the cell centre returns that cell and the same value."""
    ref, path = grid(name)
    h = ec.codes_new_from_message(ref.raw_bytes)
    rng = np.random.default_rng(1234)
    checked = odd_checked = 0
    # ecCodes' nearest-point search costs ~1 s per call on a CONUS-sized grid (it is not cached), so sample sparsely there
    n_calls = 14 if ref.n_cells > 1_000_000 else 80
    try:
        for k in range(n_calls):
            i, j = int(rng.integers(0, ref.ni)), int(rng.integers(0, ref.nj))
            if k % 2:  # alternate between odd and even rows so both parities are always covered
                j = j | 1 if (j | 1) < ref.nj else j - 1
            else:
                j = j & ~1
            if np.isnan(ref.values[j, i]):
                continue
            r = ec.codes_grib_find_nearest(h, float(ref.lat[j, i]), float(ref.lon[j, i]) % 360.0)[0]
            assert int(r.index) == j * ref.ni + i, f"cell ({i},{j}): ecCodes says flat index {r.index}"
            assert abs(float(r.value) - ref.values[j, i]) <= 1e-4 * max(1.0, abs(r.value)), (
                f"{name} cell ({i},{j}) row parity {j % 2}: ecCodes value {r.value}, reference {ref.values[j, i]}"
            )
            checked += 1
            odd_checked += j % 2
    finally:
        ec.codes_release(h)
    assert checked >= 8 and odd_checked >= 4, "sample too small to be meaningful"


@pytest.mark.parametrize("name", GRID_NAMES)
def test_reference_geometry_matches_eccodes(name, grid):
    ref, _ = grid(name)
    h = ec.codes_new_from_message(ref.raw_bytes)
    try:
        lat = ec.codes_get_array(h, "latitudes").reshape(ref.nj, ref.ni)
        lon = ec.codes_get_array(h, "longitudes").reshape(ref.nj, ref.ni)
    finally:
        ec.codes_release(h)
    assert np.abs(lat - ref.lat).max() < 1e-6
    assert np.abs(wrap180(lon - ref.lon)).max() < 1e-6


@pytest.mark.parametrize("name", GRID_NAMES)
def test_row_order_fix_is_only_needed_for_flat_values(name, grid):
    """Documents the cfgrib finding: odd rows come back mirrored only when cfgrib returns a flat 'values' dimension."""
    ref, _ = grid(name)
    if ref.row_fix_applied:
        assert ref.herbie_dims == ("values",), "row repair was needed on a 2-D cfgrib field: investigate"
    if ref.herbie_dims == ("values",) and ref.alt_row_scanning and not ref.row_fix_applied:
        pytest.skip("cfgrib now returns correctly ordered values for flat 'values' grids; the repair is a no-op")


def test_mask_counts(grid):
    ref, _ = grid("gu_masked")
    assert int(np.isnan(ref.values).sum()) == 400
    for name in ("co", "co_complex", "ak", "hi", "pr", "gu"):
        r, _ = grid(name)
        assert not r.has_mask, f"{name} has no bitmap but the reference has NaNs"


@pytest.mark.parametrize("name", ["gu", "pr", "hi"])
def test_reference_nearest_equals_brute_force(name, grid):
    ref, _ = grid(name)
    rng = np.random.default_rng(7)
    lat = rng.uniform(ref.lat.min() - 0.3, ref.lat.max() + 0.3, 60)
    lon = rng.uniform(ref.lon.min() - 0.3, ref.lon.max() + 0.3, 60)
    ii, jj, dd = ref.nearest(lat, lon)
    for k in range(len(lat)):
        brute = haversine_km(lat[k], lon[k], ref.lat, ref.lon)
        assert abs(brute.min() - dd[k]) < 1e-9


def test_box_mask_antimeridian_and_tolerance(grid):
    ref, _ = grid("ak")
    m = ref.box_mask(170.0, 50.0, -170.0, 70.0)  # lon_max < lon_min crosses the antimeridian
    direct = ((ref.lon >= 170.0) | (ref.lon <= -170.0)) & (ref.lat >= 50.0) & (ref.lat <= 70.0)
    assert np.array_equal(m, direct)
    same = ref.box_mask(170.0, 50.0, 190.0, 70.0)  # same box, 0..360 convention
    assert np.array_equal(m, same)
    assert ref.box_mask(-180, -90, 180, 90).all() and ref.box_mask(0, -90, 360, 90).all()
    grown, shrunk = ref.box_mask(170.0, 50.0, -170.0, 70.0, eps=1e-3), ref.box_mask(170.0, 50.0, -170.0, 70.0, eps=-1e-3)
    assert (shrunk <= m).all() and (m <= grown).all()


def test_single_message_fixtures_and_multi_split():
    for name, (path, mno, _) in GRIDS.items():
        spans = split_messages(path.read_bytes())
        assert len(spans) >= mno, name
    from reference import MULTI

    assert len(split_messages(MULTI.read_bytes())) == 4


def test_herbie_times(grid):
    ref, _ = grid("co")
    assert (ref.valid_time - ref.reference_time).total_seconds() == 3600


def test_herbie_xarray_path_per_grid(grid):
    """Records which grids Herbie.xarray() can open itself (Lambert, polar stereographic) and which need the cfgrib
    fallback (Mercator)."""
    via = {name: grid(name)[0].via for name in GRID_NAMES}
    for name, proj in ((n, GRIDS[n][2]) for n in GRID_NAMES):
        if proj in ("lambert", "polar_stereographic"):
            assert via[name] == "Herbie.xarray", (name, via[name])
