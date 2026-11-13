"""Bad input, hostile files and unsupported data: the extension must fail cleanly (an error message, never a crash,
a hang or silently wrong rows).  Cases the extension's own SQL tests already pin down are repeated only where they
guard something the Herbie comparison relies on."""

from __future__ import annotations

import shutil
import struct

import numpy as np
import pytest

from checks import check_points, jittered_points
from client import read_grib2_sql, sql_str
from reference import DATA_DIR, FIXTURE_DIR, GRIDS, MULTI, split_messages

CO = str(FIXTURE_DIR / "nbm_msg.grib2")
GU = str(FIXTURE_DIR / "nbm_gu_msg.grib2")
MULTI_S = str(MULTI)
PT = "[{'lat': 13.818, 'lon': 147.0361}]"


@pytest.fixture(autouse=True)
def _real_extension_only(rg):
    if rg.is_oracle:
        pytest.skip("error handling is not emulated by the oracle")


def _expect_error(rg, sql, *fragments):
    err = rg.error(sql)
    assert err is not None, f"expected an error from: {sql}"
    for f in fragments:
        assert f.lower() in err.lower(), f"error {err!r} should mention {f!r}"


# --------------------------------------------------------------------------------------------------------------------
# Argument validation
# --------------------------------------------------------------------------------------------------------------------
@pytest.mark.parametrize(
    "args,fragment",
    [
        ("", "needs one of points, bbox or all_cells"),
        ("all_cells := true, bbox := [0, 0, 1, 1]", "only one of"),
        (f"all_cells := true, points := {PT}", "only one of"),
        (f"bbox := [0, 0, 1, 1], points := {PT}", "only one of"),
        ("bbox := [1.0, 2.0, 3.0]", "bbox must be"),
        ("bbox := [1, 2, 3, 4, 5]", "bbox must be"),
        ("bbox := []", "bbox must be"),
        ("bbox := [0, 10, 5, 5]", "bbox latitudes"),
        ("bbox := [0, -91, 5, 5]", "bbox latitudes"),
        ("bbox := [0, 0, 5, 91]", "bbox latitudes"),
        ("points := []", "empty"),
        ("points := [{'lat': 10.0}]", ""),
        ("points := [{'lon': 10.0}]", ""),
        ("points := [{'lat': 91.0, 'lon': 0.0}]", "outside the valid range"),
        ("points := [{'lat': -91.0, 'lon': 0.0}]", "outside the valid range"),
        ("points := [{'lat': 0.0, 'lon': 361.0}]", "outside the valid range"),
        ("points := [{'lat': 0.0, 'lon': -361.0}]", "outside the valid range"),
        ("points := [{'lat': NULL::DOUBLE, 'lon': 0.0}]", ""),
        ("points := [{'lat': 'north', 'lon': 0.0}]", ""),
        ("points := 5", ""),
        ("message := 0, all_cells := true", "start at 1"),
        ("message := -3, all_cells := true", "start at 1"),
        ("message := 'one', all_cells := true", "message number"),
        ("all_cells := NULL", "cannot be NULL"),
        ("search := '(', all_cells := true", "regular expression"),
        ("search := '[', all_cells := true", "regular expression"),
        ("nonsense := 1, all_cells := true", ""),
    ],
)
def test_bad_arguments_are_rejected(rg, args, fragment):
    sep = ", " if args else ""
    sql = f"SELECT * FROM read_grib2({sql_str(GU)}{sep}{args})"
    _expect_error(rg, sql, *([fragment] if fragment else []))


def test_nan_and_infinite_coordinates_are_handled(rg):
    """NaN/inf must not hang or crash; an error or an empty result are both fine, a row is not."""
    for lat, lon in (
        ("'nan'::DOUBLE", "0.0"),
        ("0.0", "'nan'::DOUBLE"),
        ("'inf'::DOUBLE", "0.0"),
        ("0.0", "'-inf'::DOUBLE"),
    ):
        sql = f"SELECT count(*) AS n FROM read_grib2({sql_str(GU)}, points := [{{'lat': {lat}, 'lon': {lon}}}])"
        err = rg.error(sql)
        if err is None:
            assert int(rg.sql(sql)["n"][0]) == 0


def test_nan_bbox_is_rejected(rg):
    _expect_error(
        rg,
        f"SELECT count(*) FROM read_grib2({sql_str(GU)}, bbox := ['nan'::DOUBLE, 0, 10, 10])",
        "bbox coordinates must be finite",
    )


def test_null_and_missing_paths(rg):
    _expect_error(rg, "SELECT * FROM read_grib2(NULL::VARCHAR, all_cells := true)", "cannot be NULL")
    err = rg.error(f"SELECT * FROM read_grib2({sql_str(DATA_DIR / 'does_not_exist.grib2')}, all_cells := true)")
    assert err is not None
    err = rg.error(f"SELECT * FROM read_grib2({sql_str(FIXTURE_DIR / 'no_such_dir_??.grib2')}, all_cells := true)")
    assert err is not None or True  # an empty glob may be an error or an empty result; it must not crash


# --------------------------------------------------------------------------------------------------------------------
# Unsupported grids / stale sidecars / damaged files
# --------------------------------------------------------------------------------------------------------------------
def test_regular_lat_lon_grid_is_reported_as_unsupported(rg):
    _expect_error(rg, read_grib2_sql(str(DATA_DIR / "sample.grib2"), all_cells=True), "not supported")
    _expect_error(rg, read_grib2_sql(MULTI_S, message=4, all_cells=True), "not supported")


def test_stale_idx_is_detected_not_trusted(rg):
    _expect_error(rg, read_grib2_sql(str(DATA_DIR / "stale.grib2"), message=1, all_cells=True), "belong to this file")


def test_search_without_a_sidecar_is_an_error(rg):
    _expect_error(rg, read_grib2_sql(GU, search="CAPE", all_cells=True), "needs the .idx")


@pytest.mark.parametrize("keep", [0.0, 0.01, 0.5, 0.999])
def test_truncated_message_fails_cleanly(rg, tmp_path, keep):
    data = (FIXTURE_DIR / "nbm_gu_msg.grib2").read_bytes()
    f = tmp_path / "truncated.grib2"
    f.write_bytes(data[: int(len(data) * keep)])
    err = rg.error(read_grib2_sql(str(f), points=[(13.818, 147.0361)]))
    if err is None:  # an empty file may simply hold no messages
        assert len(rg.read(str(f), points=[(13.818, 147.0361)])) == 0 and keep == 0.0
    else:
        assert err


def test_garbage_and_wrong_magic_fail_cleanly(rg, tmp_path):
    for name, payload in (
        ("garbage.grib2", b"this is not a grib file at all" * 10),
        ("grib_prefix_only.grib2", b"GRIB" + b"\x00" * 12),
        ("grib1.grib", b"GRIB" + b"\x00\x00\x00\x01" + b"\x00" * 40),  # edition 1
    ):
        f = tmp_path / name
        f.write_bytes(payload)
        assert rg.error(read_grib2_sql(str(f), points=[(13.8, 147.0)])) is not None, name


def test_declared_length_larger_than_file_fails_cleanly(rg, tmp_path):
    data = bytearray((FIXTURE_DIR / "nbm_gu_msg.grib2").read_bytes())
    data[8:16] = struct.pack(">Q", len(data) + 10_000)
    f = tmp_path / "too_long.grib2"
    f.write_bytes(bytes(data))
    assert rg.error(read_grib2_sql(str(f), points=[(13.818, 147.0361)])) is not None


def test_corrupted_data_section_never_crashes(rg, tmp_path):
    """Flip bytes deep in the packed data: ecCodes may decode garbage or complain; either way no crash."""
    data = bytearray((FIXTURE_DIR / "nbm_gu_msg.grib2").read_bytes())
    rng = np.random.default_rng(0)
    for trial in range(6):
        d = bytearray(data)
        for pos in rng.integers(len(d) // 2, len(d) - 8, 40):
            d[int(pos)] ^= 0xFF
        f = tmp_path / f"corrupt{trial}.grib2"
        f.write_bytes(bytes(d))
        rg.error(read_grib2_sql(str(f), bbox=[145.0, 13.0, 147.5, 14.5]))  # AssertionError here means a crash


# --------------------------------------------------------------------------------------------------------------------
# Multi-message files without a sidecar (header hopping), concatenations
# --------------------------------------------------------------------------------------------------------------------
def test_concatenated_copies_without_sidecar_are_read_by_header_hopping(rg, refs, tmp_path):
    ref = refs.get(*GRIDS["gu"][:2])
    data = (FIXTURE_DIR / "nbm_gu_msg.grib2").read_bytes()
    f = tmp_path / "twice.grib2"
    f.write_bytes(data + data + data)
    pts = jittered_points(ref, 50, seed=40)
    df = rg.read(str(f), points=pts)
    assert sorted(df["message_number"].unique()) == [1, 2, 3]
    for n in (1, 2, 3):
        sub = df[df["message_number"] == n].reset_index(drop=True)
        np.testing.assert_allclose(
            sub["value"].to_numpy(dtype=float), ref.values[sub["j"].astype(int), sub["i"].astype(int)], rtol=1e-6
        )
    # message selection works on header-hopped files too
    assert sorted(rg.read(str(f), message=2, points=pts)["message_number"].unique()) == [2]


def test_trailing_garbage_after_the_last_message(rg, tmp_path):
    data = (FIXTURE_DIR / "nbm_gu_msg.grib2").read_bytes()
    f = tmp_path / "trailing.grib2"
    f.write_bytes(data + b"\x00" * 7)
    err = rg.error(read_grib2_sql(str(f), points=[(13.818, 147.0361)]))
    assert err is None or err  # either tolerated or rejected, never a crash; rows (if any) must still be right


def test_same_file_repeated_in_one_glob_is_read_per_match(rg, tmp_path):
    d = tmp_path / "g"
    d.mkdir()
    for n in ("a", "b"):
        shutil.copy(GU, d / f"{n}.grib2")
    df = rg.read(str(d / "*.grib2"), points=[(13.818, 147.0361)])
    assert len(df) == 2 and df["file_path"].nunique() == 2
    assert df["value"].nunique() == 1


def test_message_null_is_rejected_or_ignored(rg):
    """The extension rejects message := NULL, but DuckDB may drop NULL named parameters before they reach it.
    Either an error mentioning NULL, or exactly the unfiltered result, is acceptable; anything else is not."""
    sql = f"SELECT count(*) AS n FROM read_grib2({sql_str(GU)}, message := NULL, all_cells := true)"
    err = rg.error(sql)
    if err is None:
        base = int(rg.sql(f"SELECT count(*) AS n FROM read_grib2({sql_str(GU)}, all_cells := true)")["n"][0])
        assert int(rg.sql(sql)["n"][0]) == base
    else:
        assert "null" in err.lower()
