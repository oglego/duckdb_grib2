"""read_grib2(all_cells := true): every cell of every fixture, against Herbie's lat/lon/value arrays."""

from __future__ import annotations

import numpy as np
import pytest

from checks import assert_cells_match_reference
from conftest import GRID_NAMES, SMALL_GRIDS

BIG = ["co", "co_complex", "ak"]


def _check_complete_grid(df, ref, name):
    n = ref.n_cells
    assert len(df) == n, f"{name}: {len(df)} rows for a {ref.ni} x {ref.nj} grid ({n} cells)"
    flat = df["j"].to_numpy(dtype=np.int64) * ref.ni + df["i"].to_numpy(dtype=np.int64)
    assert (flat == np.arange(n)).all(), f"{name}: rows are not in grid order (i fastest, then j) or cells are missing"
    assert_cells_match_reference(df, ref, name)


@pytest.mark.parametrize("name", SMALL_GRIDS)
def test_every_cell_matches_herbie(name, grid, rg):
    ref, path = grid(name)
    df = rg.read(path, all_cells=True, select="message_number, reference_time, valid_time, lat, lon, i, j, value")
    _check_complete_grid(df, ref, name)


@pytest.mark.slow
@pytest.mark.parametrize("name", BIG)
def test_every_cell_matches_herbie_big_grids(name, grid, rg):
    """Full comparison of 1.8M (Alaska) and 3.7M (CONUS) cells. Needs a few GB of RAM and a minute or two."""
    ref, path = grid(name)
    df = rg.read(path, all_cells=True, select="message_number, reference_time, valid_time, lat, lon, i, j, value")
    _check_complete_grid(df, ref, name)


@pytest.mark.parametrize("name", BIG)
@pytest.mark.parametrize("modulus,remainder", [(997, 3), (1009, 0)])
def test_strided_sample_of_big_grids(name, modulus, remainder, grid, rg):
    """Every ~1000th cell by flat index (prime strides hit every column and both row parities)."""
    ref, path = grid(name)
    df = rg.read(path, all_cells=True, flat_mod=(ref.ni, modulus, remainder),
                 select="message_number, reference_time, valid_time, lat, lon, i, j, value")  # fmt: skip
    flat = np.arange(ref.n_cells)
    expect = flat[flat % modulus == remainder]
    got = df["j"].to_numpy(dtype=np.int64) * ref.ni + df["i"].to_numpy(dtype=np.int64)
    assert (got == expect).all(), f"{name}: sampled cells differ from expected flat indices"
    assert_cells_match_reference(df, ref, f"{name} stride {modulus}")
    assert set((df["j"] % 2).unique()) == {0, 1}


@pytest.mark.parametrize("name", GRID_NAMES)
def test_aggregates_match_herbie(name, grid, rg):
    """count / count(value) / sum / min / max over the full grid, computed inside DuckDB (no big transfer)."""
    if rg.is_oracle:
        pytest.skip("aggregate SQL not emulated by the oracle")
    ref, path = grid(name)
    from client import read_grib2_sql

    agg = rg.sql(read_grib2_sql(path, all_cells=True, select=(
        "count(*) AS n, count(value) AS n_valid, sum(value) AS s, min(value) AS mn, max(value) AS mx, "
        "count(DISTINCT (i, j)) AS n_distinct, min(i) AS i0, max(i) AS i1, min(j) AS j0, max(j) AS j1")))  # fmt: skip
    r = agg.iloc[0]
    assert int(r["n"]) == ref.n_cells and int(r["n_distinct"]) == ref.n_cells
    assert (int(r["i0"]), int(r["i1"]), int(r["j0"]), int(r["j1"])) == (0, ref.ni - 1, 0, ref.nj - 1)
    valid = ref.values[~np.isnan(ref.values)]
    assert int(r["n_valid"]) == valid.size, "number of non-NULL values differs (masked cells must be NULL)"
    assert float(r["s"]) == pytest.approx(float(valid.sum()), rel=1e-6)
    assert float(r["mn"]) == pytest.approx(float(valid.min()), rel=1e-6, abs=1e-6)
    assert float(r["mx"]) == pytest.approx(float(valid.max()), rel=1e-6, abs=1e-6)


def test_masked_grid_has_exactly_the_masked_cells_null(grid, rg):
    ref, path = grid("gu_masked")
    df = rg.read(path, all_cells=True, select="i, j, value, message_number")
    null = df["value"].isna().to_numpy()
    assert null.sum() == 400
    assert (null == np.isnan(ref.values).ravel()).all(), "NULL values are not exactly the bitmap-masked cells"
