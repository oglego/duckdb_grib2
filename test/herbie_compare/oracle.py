"""A stand-in for the extension, used to test the test suite itself.

``GRIB2_ORACLE=1 pytest test/herbie_compare`` runs the spatial tests against this module instead of the compiled
extension.  It answers ``read(...)`` calls from the Herbie-derived reference, so the tests should all pass, which
proves the harness (column names, dtypes, tolerances, ordering rules) is self-consistent.  Setting
``GRIB2_ORACLE_BUG=<name>`` injects a realistic implementation bug; the matching tests must then FAIL, which proves
they can detect it:

    mirror_odd_rows   use ecCodes' stored value order for odd rows (the trap described in docs/DESIGN.md)
    off_by_one_i      report i one cell too far east (and the value of that cell)
    no_lon_wrap       treat 0..360 longitudes as if they were -180..180
    open_bbox_edges   treat bbox edges as exclusive
    drop_masked       skip masked cells instead of returning NULL values
    unsorted_bbox     return bbox cells in descending grid order

Only spatial reads of the fixtures in ``reference.GRIDS`` are supported; anything else skips the test.
"""

from __future__ import annotations

import os
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from reference import RefCache, RefField, wrap180

COLUMNS = [
    "file_path", "message_number", "reference_time", "valid_time", "variable", "level", "forecast", "qualifier",
    "short_name", "lat", "lon", "i", "j", "value", "query_lat", "query_lon", "distance_km",
]  # fmt: skip


class OracleClient:
    is_oracle = True

    def __init__(self, cache: RefCache):
        self.cache = cache
        self.bug = os.environ.get("GRIB2_ORACLE_BUG", "")

    # unsupported surface -> skip
    def sql(self, query):
        pytest.skip("oracle mode only emulates read() for spatial queries")

    def error(self, query):
        pytest.skip("oracle mode does not emulate error handling")

    def index(self, path, **kwargs):
        pytest.skip("oracle mode does not emulate grib2_index")

    # ------------------------------------------------------------------------------------------------------------
    def _rows(self, ref: RefField, ii, jj, qlat=None, qlon=None, dist=None) -> pd.DataFrame:
        ii = np.asarray(ii, dtype=np.int64)
        jj = np.asarray(jj, dtype=np.int64)
        n = len(ii)
        vals = ref.values.copy()
        if self.bug == "mirror_odd_rows":
            vals[1::2] = vals[1::2, ::-1]
        if self.bug == "off_by_one_i":
            ii2 = np.minimum(ii + 1, ref.ni - 1)
            v = vals[jj, ii2]
        else:
            v = vals[jj, ii]
        df = pd.DataFrame(
            {
                "file_path": ref.source,
                "message_number": ref.message_number,
                "reference_time": ref.reference_time,
                "valid_time": ref.valid_time if Path(ref.source + ".idx").exists() else pd.NaT,
                "lat": ref.lat[jj, ii],
                "lon": ref.lon[jj, ii],
                "i": ii,
                "j": jj,
                "value": v,
            }
        )
        for c in ("variable", "level", "forecast", "qualifier", "short_name"):
            df[c] = None
        df["query_lat"] = qlat if qlat is not None else np.nan
        df["query_lon"] = qlon if qlon is not None else np.nan
        df["distance_km"] = dist if dist is not None else np.nan
        assert len(df) == n
        return df[COLUMNS]

    def read(self, path, *, message=None, search=None, points=None, bbox=None, all_cells=False, select=None,
             where=None, order_by=None, flat_mod=None, **_):  # fmt: skip
        if search is not None or where or order_by:
            pytest.skip("oracle mode: unsupported read() options")
        match = [(k, v) for k, v in __import__("reference").GRIDS.items() if str(v[0]) == str(Path(path))]
        if not match:
            pytest.skip("oracle mode only knows the single-message fixtures")
        _, (fpath, mno, _) = match[0]
        ref = self.cache.get(fpath, mno)
        if message is not None and mno not in ([message] if isinstance(message, int) else list(message)):
            return pd.DataFrame(columns=COLUMNS)

        if points is not None:
            plat = np.array([p[0] for p in points], dtype=float)
            plon = np.array([p[1] for p in points], dtype=float)
            qlon = plon.copy()
            if self.bug == "no_lon_wrap":
                plon = np.where(plon > 180, 1e9, plon)  # nothing sensible comes out of an unwrapped 0..360 longitude
            i, j, d = ref.nearest(plat, wrap180(plon) if self.bug != "no_lon_wrap" else plon)
            kind = (
                ref.classify_point(plat, plon)
                if self.bug != "no_lon_wrap"
                else ["outside" if x > 180 else "inside" for x in plon]
            )

            # Footprint rule: ambiguous points are in the grid unless they sit beyond half a cell off an edge cell
            def _in(k, dd, a_, b_):
                if k == "inside":
                    return True
                if k == "outside":
                    return False
                on_edge = a_ in (0, ref.ni - 1) or b_ in (0, ref.nj - 1)
                return (not on_edge) or dd <= 0.5 * ref.cell_spacing_km(int(a_), int(b_))

            keep = np.array([_in(k, dd, int(a_), int(b_)) for k, dd, a_, b_ in zip(kind, d, i, j)], dtype=bool)
            return self._rows(ref, i[keep], j[keep], plat[keep], qlon[keep], d[keep])

        if bbox is not None:
            lon_min, lat_min, lon_max, lat_max = bbox
            if self.bug == "no_lon_wrap":
                lon_min, lon_max = wrap180(lon_min) if lon_min <= 180 else 1e9, (
                    wrap180(lon_max) if lon_max <= 180 else 1e9
                )
            mask = ref.box_mask(lon_min, lat_min, lon_max, lat_max)
            if self.bug == "open_bbox_edges":
                mask &= ref.box_mask(lon_min, lat_min, lon_max, lat_max, eps=-1e-6)
            jj, ii = np.nonzero(mask)  # row-major -> ascending flat index
            if self.bug == "unsorted_bbox":
                jj, ii = jj[::-1], ii[::-1]
            return self._rows(ref, ii, jj)

        if all_cells:
            jj, ii = np.divmod(np.arange(ref.n_cells), ref.ni)
            if flat_mod:
                _, mod, rem = flat_mod
                keep = (np.arange(ref.n_cells) % mod) == rem
                ii, jj = ii[keep], jj[keep]
            df = self._rows(ref, ii, jj)
            if self.bug == "drop_masked":
                df = df[df["value"].notna()]
            return df
        pytest.skip("oracle mode: read() needs points, bbox or all_cells")
