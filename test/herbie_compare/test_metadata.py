"""Metadata columns, message selection, search and the scalar helpers, compared with Herbie's inventory/fields.

Herbie parses the same wgrib2-style .idx files, so ``Herbie.inventory()`` is an independent reading of them.
"""

from __future__ import annotations

import re
import shutil
import warnings

import numpy as np
import pandas as pd
import pytest

from checks import check_points
from client import read_grib2_sql, sql_str
from reference import DATA_DIR, FIXTURE_DIR, GRIDS, MULTI, MULTI_IDX, split_messages

NBM_IDX = FIXTURE_DIR / "blend.t00z.core.f001.co.grib2.idx"
NBM_FILE_SIZE = 156464859  # size of the original CONUS file in the NBM bucket; the .idx cannot say how long the last message is
REF_DATE = "2026-10-23 00:00"


def _herbie_inventory(workdir, grib_path, idx_path):
    """Lay the files out the way Herbie's NBM template expects and return Herbie's inventory DataFrame."""
    from herbie import Herbie

    d = workdir / "nbm" / "20261023"
    d.mkdir(parents=True, exist_ok=True)
    target = d / "blend.t00z.core.f001.co.grib2"
    if grib_path is None:
        with open(target, "wb") as f:
            f.truncate(NBM_FILE_SIZE)  # sparse file: only its size matters
    else:
        shutil.copy(grib_path, target)
    shutil.copy(idx_path, str(target) + ".idx")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        H = Herbie(REF_DATE, model="nbm", product="co", fxx=1, save_dir=workdir, verbose=False)
        assert H.idx_source == "local"
        return target, H.inventory()


def _herbie_qualifier(inv):
    """Herbie splits everything after the forecast field into the columns '?', '??', '???'; rejoin them."""
    cols = [c for c in ("?", "??", "???") if c in inv]
    out = []
    for row in inv[cols].itertuples(index=False):
        parts = [x for x in row if isinstance(x, str) and x != ""]
        out.append(":".join(parts) or None)
    return out


def _nullable(series):
    return [x if isinstance(x, str) else None for x in series]


@pytest.fixture(scope="module")
def nbm_idx_case(tmp_path_factory):
    work = tmp_path_factory.mktemp("nbm_idx")
    target, inv = _herbie_inventory(work, None, NBM_IDX)
    return target, inv


@pytest.fixture(scope="module")
def multi_case(tmp_path_factory):
    work = tmp_path_factory.mktemp("multi")
    target, inv = _herbie_inventory(work, MULTI, MULTI_IDX)
    return target, inv


# ---------------------------------------------------------------------------------------------------------------------
# grib2_index vs Herbie's reading of the .idx
# ---------------------------------------------------------------------------------------------------------------------
def test_index_of_the_real_nbm_inventory_matches_herbie(nbm_idx_case, rg):
    path, inv = nbm_idx_case
    idx = rg.index(str(path))
    assert len(idx) == len(inv) == 154
    assert (idx["message_number"].to_numpy() == inv["grib_message"].to_numpy()).all()
    assert (idx["byte_offset"].to_numpy() == inv["start_byte"].to_numpy()).all()
    ends = inv["end_byte"].to_numpy(dtype=float)
    ends[-1] = NBM_FILE_SIZE - 1  # Herbie cannot know the last message's end; the extension uses the file size
    assert ((idx["byte_offset"] + idx["byte_length"] - 1).to_numpy() == ends).all(), "byte ranges differ from Herbie's"
    for ours, theirs in (("variable", "variable"), ("level", "level"), ("forecast", "forecast_time")):
        assert (idx[ours].to_numpy() == inv[theirs].to_numpy()).all(), f"{ours} differs from Herbie's {theirs}"
    assert (idx["inventory"].to_numpy() == inv["search_this"].str.strip(":").to_numpy()).all()
    assert _nullable(idx["qualifier"]) == _herbie_qualifier(inv)
    assert (pd.to_datetime(idx["reference_time"]) == pd.to_datetime(inv["reference_time"])).all()


def test_index_percentile_and_threshold_columns_agree_with_herbie_text(nbm_idx_case, rg):
    path, inv = nbm_idx_case
    idx = rg.index(str(path))
    qual = _herbie_qualifier(inv)
    for k, q in enumerate(qual):
        pm = re.fullmatch(r"([\d.]+)% level", q or "")
        if pm:
            assert idx.loc[k, "percentile"] == pytest.approx(float(pm.group(1))), f"message {k + 1}"
        else:
            assert pd.isna(idx.loc[k, "percentile"]), f"message {k + 1} has no percentile"
    assert (idx["step_hours"].dropna() == 1).all()


def test_index_of_multi_file_matches_herbie_and_header_hopping(multi_case, rg):
    path, inv = multi_case
    with_idx = rg.index(str(path))
    assert len(with_idx) == len(inv) == 4
    assert list(with_idx["variable"]) == list(inv["variable"])
    assert list(with_idx["forecast"]) == list(inv["forecast_time"])
    assert list(with_idx["byte_offset"]) == list(inv["start_byte"])
    # header hopping (ignoring the sidecar) must find the same message boundaries as a byte-level split
    hop = rg.index(str(path), idx=False)
    spans = split_messages(MULTI.read_bytes())
    assert list(hop["message_number"]) == [1, 2, 3, 4]
    assert list(hop["byte_offset"]) == [o for o, _ in spans]
    assert list(hop["byte_length"]) == [n for _, n in spans]
    assert hop["variable"].isna().all(), "without a sidecar the names are NULL"
    assert list(hop["byte_offset"]) == list(with_idx["byte_offset"])


@pytest.mark.parametrize("name", ["co", "ak", "hi", "pr", "gu", "co_complex"])
def test_index_of_single_message_files_without_sidecar(name, rg):
    path = GRIDS[name][0]
    hop = rg.index(str(path))
    assert len(hop) == 1 and int(hop["byte_offset"][0]) == 0 and int(hop["byte_length"][0]) == path.stat().st_size


# ---------------------------------------------------------------------------------------------------------------------
# read_grib2 metadata columns, message selection and search
# ---------------------------------------------------------------------------------------------------------------------
def _multi_points(refs_):
    """One centre point from each of the Guam, Puerto Rico and Hawaii grids: each lands in exactly one message."""
    out = []
    for n in ("gu", "pr", "hi"):
        r = refs_.get(*GRIDS[n][:2])
        out.append((float(r.lat[r.nj // 2, r.ni // 2]), float(r.lon[r.nj // 2, r.ni // 2])))
    return out


def test_read_metadata_columns_match_herbie(multi_case, refs, rg):
    path, inv = multi_case
    pts = _multi_points(refs)
    df = rg.read(str(MULTI), message=[1, 2, 3], points=pts)
    assert list(df["message_number"]) == [1, 2, 3]
    for _, row in df.iterrows():
        h = inv[inv["grib_message"] == row["message_number"]].iloc[0]
        assert row["variable"] == h["variable"] and row["level"] == h["level"] and row["forecast"] == h["forecast_time"]
        assert pd.Timestamp(row["reference_time"]) == pd.Timestamp(h["reference_time"])
        assert pd.Timestamp(row["valid_time"]) == pd.Timestamp(h["valid_time"]), "valid_time is reference time + step"
    for k, n in enumerate(("gu", "pr", "hi")):  # and the values themselves, through Herbie's fields
        ref = refs.get(*GRIDS[n][:2])
        sub = df[df["message_number"] == k + 1]
        assert pd.Timestamp(sub["valid_time"].iloc[0]) == ref.valid_time
        np.testing.assert_allclose(sub["value"].to_numpy(dtype=float), ref.values[int(sub["j"].iloc[0]), int(sub["i"].iloc[0])], rtol=1e-6)


def test_message_selection(refs, rg):
    pts = _multi_points(refs)
    one = rg.read(str(MULTI), message=2, points=pts)
    assert list(one["message_number"]) == [2]
    assert list(rg.read(str(MULTI), message=[1, 3], points=pts)["message_number"]) == [1, 3]
    # messages come back in file order whatever the order of the list
    assert list(rg.read(str(MULTI), message=[3, 1], points=pts)["message_number"]) == [1, 3]
    # all messages by default (message 4 is regular lat/lon and unsupported, so ask only for the first three)
    assert list(rg.read(str(MULTI), message=[1, 2, 3], points=pts)["message_number"]) == [1, 2, 3]


def test_message_number_beyond_the_file_is_graceful(refs, rg):
    pts = _multi_points(refs)
    err = rg.error(read_grib2_sql(str(MULTI), message=99, points=pts))
    if err is None:
        assert len(rg.read(str(MULTI), message=99, points=pts)) == 0
    else:
        assert err  # a clear error is also acceptable; what matters is no crash and no wrong rows


SEARCHES = [
    "APTMP", ":APTMP:2 m above ground:", ":PTMP:", "^CAPE:", "CAPE|APTMP", "aptmp", "2 m above ground", r"\d+ hour fcst$",
    "^APTMP:2 m above ground:1 hour fcst$", "surface", "T:surface:anl", "NOSUCHVAR",
]  # fmt: skip


@pytest.mark.parametrize("pattern", SEARCHES)
def test_search_matches_the_same_messages_as_a_regex_over_herbies_inventory(pattern, multi_case, refs, rg):
    """read_grib2 matches the pattern against the inventory text as is, and with colons around it."""
    path, inv = multi_case
    pts = _multi_points(refs)
    texts = inv["search_this"].str.strip(":").tolist()
    expect = [n for n, t in zip(inv["grib_message"], texts) if re.search(pattern, t) or re.search(pattern, ":" + t + ":")]
    expect = [n for n in expect if n in (1, 2, 3)]  # message 4 is unsupported by read_grib2
    sql = read_grib2_sql(str(MULTI), search=pattern, message=[1, 2, 3], points=pts, select="DISTINCT message_number", order_by="message_number")
    got = [int(x) for x in rg.sql(sql)["message_number"]] if not rg.is_oracle else None
    if got is None:
        pytest.skip("oracle mode")
    assert got == expect, f"search {pattern!r}: extension {got}, regex over Herbie's inventory {expect}"


def test_search_and_message_are_combined_with_and(refs, rg):
    pts = _multi_points(refs)
    assert len(rg.read(str(MULTI), message=3, search="CAPE", points=pts)) == 0
    assert list(rg.read(str(MULTI), message=1, search="CAPE", points=pts)["message_number"]) == [1]


def test_files_matched_by_a_glob_come_back_in_path_order(refs, rg):
    pts = {"ak": (58.0874, -154.1681), "gu": (13.818, 147.0361), "hi": (22.005, -154.4903), "pr": (17.7425, -66.4248)}
    pattern = str(FIXTURE_DIR / "nbm_??_msg.grib2")
    df = rg.read(pattern, points=list(pts.values()))
    files = list(dict.fromkeys(df["file_path"]))
    assert [f.split("nbm_")[-1][:2] for f in files] == ["ak", "gu", "hi", "pr"], "files must come back in path order"
    for f in files:
        key = f.split("nbm_")[-1][:2]
        ref = refs.get(*GRIDS[key][:2])
        sub = df[df["file_path"] == f]
        assert len(sub) >= 1 and (sub["message_number"] == 1).all()
        np.testing.assert_allclose(sub["value"].to_numpy(dtype=float), ref.values[sub["j"].astype(int), sub["i"].astype(int)], rtol=1e-6)


# ---------------------------------------------------------------------------------------------------------------------
# Scalar helpers: grib2_point_value, grib2_message_info, grib2_eccodes_version
# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize("name", ["gu", "pr", "hi", "gu_masked", "ak", "co"])
def test_grib2_point_value_matches_herbie(name, grid, rg):
    if rg.is_oracle:
        pytest.skip("scalar functions not emulated by the oracle")
    from checks import jittered_points

    ref, path = grid(name)
    n = 6 if ref.n_cells > 1_000_000 else 25  # the function decodes the whole message per call
    pts = jittered_points(ref, n, seed=21) + [(0.0, 0.0)]
    rows = ", ".join(f"({la!r}::DOUBLE, {lo!r}::DOUBLE)" for la, lo in pts)
    df = rg.sql(f"SELECT t.lat, t.lon, grib2_point_value(b.content, t.lat, t.lon) AS v FROM read_blob({sql_str(path)}) b, (VALUES {rows}) t(lat, lon)")
    got = {(round(float(a), 9), round(float(b), 9)): v for a, b, v in zip(df["lat"], df["lon"], df["v"])}
    for la, lo in pts[:-1]:
        i, j, _ = ref.nearest(la, lo)
        exp = ref.values[int(j[0]), int(i[0])]
        v = got[(round(la, 9), round(lo, 9))]
        assert (pd.isna(v) and np.isnan(exp)) or v == pytest.approx(exp, rel=1e-6, abs=1e-6), f"{name} ({la}, {lo})"
    assert pd.isna(got[(0.0, 0.0)]), "a point outside the grid gives NULL"


@pytest.mark.parametrize("name", ["gu", "pr", "hi", "gu_masked", "co_complex"])
def test_grib2_message_info_min_max_match_herbie(name, grid, rg):
    if rg.is_oracle:
        pytest.skip("scalar functions not emulated by the oracle")
    ref, path = grid(name)
    info = rg.sql(f"SELECT grib2_message_info(content) AS i FROM read_blob({sql_str(path)})")["i"][0]
    kv = dict(p.split("=", 1) for p in info.split(";"))
    assert int(kv["Ni"]) == ref.ni and int(kv["Nj"]) == ref.nj
    assert float(kv["min"]) == pytest.approx(float(np.nanmin(ref.values)), abs=1.5e-3), "min ignores masked cells"
    assert float(kv["max"]) == pytest.approx(float(np.nanmax(ref.values)), abs=1.5e-3)
    assert kv["alternativeRowScanning"] == "1"


def test_eccodes_version_is_reported(rg):
    if rg.is_oracle:
        pytest.skip("scalar functions not emulated by the oracle")
    v = rg.sql("SELECT grib2_eccodes_version() AS v")["v"][0]
    assert re.fullmatch(r"\d+\.\d+\.\d+", v)
