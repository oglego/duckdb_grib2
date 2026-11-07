"""Comparison helpers shared by the test modules. All "expected" values come from reference.RefField (Herbie)."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd

from reference import RefField, haversine_km, wrap180

VALUE_RTOL = 1e-6  # cfgrib hands back float32; the extension returns ecCodes' float64
VALUE_ATOL = 1e-6
COORD_TOL_DEG = 1e-6  # the extension's closed-form projection agrees with ecCodes to ~1e-11 degrees
DIST_TOL_KM = 1e-6
BOX_EPS_DEG = 1e-7  # cells this close to a box edge may legitimately fall on either side


def lon_diff(a, b):
    """Absolute longitude difference in degrees, safe across the +/-180 seam."""
    return np.abs(wrap180(np.asarray(a, dtype=np.float64) - np.asarray(b, dtype=np.float64)))


def assert_values_match(got, expected, context: str = ""):
    """NaN/NULL aware. ``expected`` NaN means the cell is masked and the extension must return NULL."""
    got = np.asarray(pd.to_numeric(pd.Series(got), errors="coerce"), dtype=np.float64)
    expected = np.asarray(expected, dtype=np.float64)
    assert got.shape == expected.shape, f"{context}: {got.shape} values vs {expected.shape} expected"
    g_nan, e_nan = np.isnan(got), np.isnan(expected)
    if (g_nan != e_nan).any():
        k = int(np.nonzero(g_nan != e_nan)[0][0])
        raise AssertionError(
            f"{context}: NULL-ness differs at {int((g_nan != e_nan).sum())} rows, first at row {k} "
            f"(got {got[k]}, expected {expected[k]})"
        )
    ok = np.isclose(got[~g_nan], expected[~e_nan], rtol=VALUE_RTOL, atol=VALUE_ATOL)
    if not ok.all():
        bad = np.nonzero(~g_nan)[0][~ok]
        k = int(bad[0])
        raise AssertionError(
            f"{context}: {len(bad)} of {len(got)} values differ, first at row {k}: got {got[k]!r}, expected {expected[k]!r}"
        )


def assert_cells_match_reference(df: pd.DataFrame, ref: RefField, context: str = ""):
    """Every row's (i, j) -> lat, lon, value, message metadata must agree with Herbie's field."""
    if df.empty:
        return
    i = df["i"].to_numpy(dtype=np.int64)
    j = df["j"].to_numpy(dtype=np.int64)
    assert (i >= 0).all() and (i < ref.ni).all(), f"{context}: i out of range 0..{ref.ni - 1}"
    assert (j >= 0).all() and (j < ref.nj).all(), f"{context}: j out of range 0..{ref.nj - 1}"
    dlat = np.abs(df["lat"].to_numpy(dtype=np.float64) - ref.lat[j, i])
    dlon = lon_diff(df["lon"], ref.lon[j, i])
    assert dlat.max() <= COORD_TOL_DEG, f"{context}: lat differs from Herbie by up to {dlat.max():.3g} deg"
    assert dlon.max() <= COORD_TOL_DEG, f"{context}: lon differs from Herbie by up to {dlon.max():.3g} deg"
    assert_values_match(df["value"], ref.values[j, i], f"{context}: value")
    assert (df["message_number"] == ref.message_number).all(), f"{context}: wrong message_number"
    if "reference_time" in df:
        assert (pd.to_datetime(df["reference_time"]) == ref.reference_time).all(), f"{context}: reference_time"
    if "valid_time" in df:
        # valid_time is reference time + the step read from the .idx (documented), so it is NULL without a sidecar.
        # With one it must equal Herbie's valid_time.
        vt = pd.to_datetime(df["valid_time"])
        if Path(ref.source + ".idx").exists():
            assert (vt == ref.valid_time).all(), f"{context}: valid_time {vt.iloc[0]} != Herbie's {ref.valid_time}"
        else:
            assert vt.isna().all(), f"{context}: valid_time should be NULL without an .idx sidecar, got {vt.iloc[0]}"


def check_points(rg, ref: RefField, pts, *, path=None, context: str = "", **read_kwargs) -> pd.DataFrame:
    """Run read_grib2(points := pts) and verify every point against Herbie.

    * a point certainly inside the grid must yield exactly one row; one certainly outside must yield none; points in
      the ambiguous band right at the edge may go either way (the half-cell footprint rule is the extension's policy)
    * rows come back in the order of the points, with query_lat/query_lon echoed exactly
    * the cell is the nearest by great-circle distance (a genuine tie in distance is accepted either way)
    * distance_km is the haversine distance from the query point to that cell centre
    * value is Herbie's value at that cell (NULL where masked), so mirrored odd rows are caught
    """
    path = path or ref.source
    df = rg.read(path, points=pts, **read_kwargs)
    plat = np.array([p[0] for p in pts], dtype=np.float64)
    plon = np.array([p[1] for p in pts], dtype=np.float64)
    kinds = ref.classify_point(plat, plon)
    exp_i, exp_j, exp_d = ref.nearest(plat, plon)

    rows = df.reset_index(drop=True)
    g = 0
    kept = []
    for n, (la, lo, kind) in enumerate(zip(plat, plon, kinds)):
        has_row = (
            g < len(rows)
            and abs(rows.at[g, "query_lat"] - la) <= 1e-9
            and lon_diff(rows.at[g, "query_lon"], lo) <= 1e-9
        )
        # query_lon must be echoed as given, not wrapped
        if has_row:
            assert abs(rows.at[g, "query_lon"] - lo) <= 1e-9, f"{context}: query_lon not echoed as given for point {n}"
        if kind == "inside":
            assert has_row, f"{context}: point {n} ({la}, {lo}) is inside the grid but produced no row"
        if kind == "outside":
            assert not has_row, f"{context}: point {n} ({la}, {lo}) is outside the grid but produced a row"
        if has_row:
            r = rows.loc[g]
            ri, rj = int(r["i"]), int(r["j"])
            if (ri, rj) != (int(exp_i[n]), int(exp_j[n])):
                d_ext = float(haversine_km(la, lo, ref.lat[rj, ri], ref.lon[rj, ri]))
                assert abs(d_ext - float(exp_d[n])) <= DIST_TOL_KM, (
                    f"{context}: point {n} ({la}, {lo}): extension chose cell ({ri},{rj}) at {d_ext:.6f} km but "
                    f"the nearest is ({exp_i[n]},{exp_j[n]}) at {float(exp_d[n]):.6f} km"
                )
            d_row = float(haversine_km(la, lo, ref.lat[rj, ri], ref.lon[rj, ri]))
            assert abs(float(r["distance_km"]) - d_row) <= max(
                DIST_TOL_KM, 1e-9 * d_row
            ), f"{context}: point {n}: distance_km {r['distance_km']} != haversine {d_row}"
            kept.append(g)
            g += 1
    assert g == len(rows), f"{context}: {len(rows) - g} unexpected extra rows after the last point"
    assert_cells_match_reference(rows, ref, context)
    return rows


def check_bbox(rg, ref: RefField, bbox, *, path=None, context: str = "", **read_kwargs) -> pd.DataFrame:
    """Run read_grib2(bbox := bbox) and verify the selected cell set, order and contents against Herbie.

    The extension's selection must lie between the Herbie mask of a slightly shrunk box and of a slightly grown box, so
    cells that sit within 1e-7 degrees of an edge (float noise) cannot cause spurious failures.
    """
    path = path or ref.source
    df = rg.read(path, bbox=bbox, **read_kwargs).reset_index(drop=True)
    lon_min, lat_min, lon_max, lat_max = bbox
    strict = ref.box_mask(lon_min, lat_min, lon_max, lat_max, eps=-BOX_EPS_DEG).ravel()
    loose = ref.box_mask(lon_min, lat_min, lon_max, lat_max, eps=+BOX_EPS_DEG).ravel()
    flat = (
        (df["j"].to_numpy(dtype=np.int64) * ref.ni + df["i"].to_numpy(dtype=np.int64))
        if len(df)
        else np.array([], dtype=np.int64)
    )

    assert len(np.unique(flat)) == len(flat), f"{context}: a cell was returned more than once"
    assert (np.diff(flat) > 0).all(), f"{context}: rows are not in ascending grid order (j * ni + i)"
    got = np.zeros(ref.n_cells, dtype=bool)
    got[flat] = True
    missing = np.nonzero(strict & ~got)[0]
    extra = np.nonzero(got & ~loose)[0]
    assert len(missing) == 0, (
        f"{context}: {len(missing)} cells inside the box are missing, e.g. flat index {missing[0]} "
        f"(i={missing[0] % ref.ni}, j={missing[0] // ref.ni}) at lat {ref.lat.ravel()[missing[0]]:.5f} lon {ref.lon.ravel()[missing[0]]:.5f}"
    )
    assert len(extra) == 0, (
        f"{context}: {len(extra)} cells outside the box were returned, e.g. flat index {extra[0]} "
        f"(i={extra[0] % ref.ni}, j={extra[0] // ref.ni}) at lat {ref.lat.ravel()[extra[0]]:.5f} lon {ref.lon.ravel()[extra[0]]:.5f}"
    )
    assert_cells_match_reference(df, ref, context)
    return df


# --------------------------------------------------------------------------------------------------------------------
# Point generators
# --------------------------------------------------------------------------------------------------------------------
def jittered_points(ref: RefField, n: int, seed: int, margin: int = 2, jitter: float = 0.49):
    """n random points, each inside a random cell (at least ``margin`` cells from the edge), offset by up to ``jitter``
    of a cell along both grid axes (measured with the neighbouring cell centres)."""
    rng = np.random.default_rng(seed)
    i = rng.integers(margin, ref.ni - margin, n)
    j = rng.integers(margin, ref.nj - margin, n)
    u = rng.uniform(-jitter, jitter, n)
    v = rng.uniform(-jitter, jitter, n)
    lat = ref.lat[j, i] + u * (ref.lat[j, i + 1] - ref.lat[j, i]) + v * (ref.lat[j + 1, i] - ref.lat[j, i])
    dlon_i = wrap180(ref.lon[j, i + 1] - ref.lon[j, i])
    dlon_j = wrap180(ref.lon[j + 1, i] - ref.lon[j, i])
    lon = ref.lon[j, i] + u * dlon_i + v * dlon_j
    return [(float(a), float(b)) for a, b in zip(lat, wrap180(lon))]


def to_360(lon: float) -> float:
    return float(lon % 360.0)
