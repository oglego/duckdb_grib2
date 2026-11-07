"""Ground truth for the DuckDB-vs-Herbie comparison tests.

Everything the extension returns is compared against what *Herbie* (which opens GRIB2 through cfgrib/ecCodes) reports
for the same bytes. Herbie is pointed at local copies of the fixture messages, so no network access is needed.

Two limitations of Herbie/cfgrib that this module works around (and the tests document):
  * ``Herbie.xarray()`` raises NotImplementedError for Mercator grids; those messages are read with cfgrib using the
    same arguments Herbie uses (``RefField.via`` records which path was taken).
  * cfgrib mirrors odd rows on the flat ``values`` grids (see below).

IMPORTANT: Herbie/cfgrib is not a perfect oracle for NBM data. These messages use alternative row scanning (odd rows
are stored east-to-west). cfgrib corrects this when it reshapes a grid to 2-D (Lambert, polar stereographic), but for
grids it hands back as a flat ``values`` dimension (the Mercator grids: Hawaii, Puerto Rico, Guam) the odd rows come
back MIRRORED. Comparing the extension against raw cfgrib output would therefore report false failures on those
grids. ``RefField`` detects this by probing ecCodes' own nearest-point lookup on odd rows, and repairs the field when
needed; ``test_reference_sanity.py`` independently verifies the final reference against ecCodes.
"""

from __future__ import annotations

import math
import shutil
import struct
import tempfile
import warnings
from dataclasses import dataclass, field
from pathlib import Path

import eccodes as ec
import numpy as np
import pandas as pd

REPO = Path(__file__).resolve().parents[2]
EARTH_RADIUS_KM = 6371.2  # shapeOfTheEarth = 1 for every NBM region (spherical, 6,371,200 m)


# --------------------------------------------------------------------------------------------------------------------
# Splitting a GRIB2 file into messages without decoding (hops by the length in the indicator section)
# --------------------------------------------------------------------------------------------------------------------
def split_messages(data: bytes) -> list[tuple[int, int]]:
    """Return [(offset, length), ...] for every GRIB2 message in ``data``."""
    out, pos = [], 0
    while pos + 16 <= len(data):
        if data[pos : pos + 4] != b"GRIB":
            raise ValueError(f"no GRIB marker at byte {pos}")
        (length,) = struct.unpack(">Q", data[pos + 8 : pos + 16])
        out.append((pos, length))
        pos += length
    return out


# --------------------------------------------------------------------------------------------------------------------
# Geometry helpers (independent of the extension's closed-form projections)
# --------------------------------------------------------------------------------------------------------------------
def wrap180(lon):
    lon = np.asarray(lon, dtype=np.float64)
    return lon - 360.0 * np.floor((lon + 180.0) / 360.0)


def haversine_km(lat1, lon1, lat2, lon2, radius_km: float = EARTH_RADIUS_KM):
    p1, p2 = np.radians(lat1), np.radians(lat2)
    dl = np.radians(np.asarray(lon2, dtype=np.float64) - np.asarray(lon1, dtype=np.float64))
    a = np.sin((p2 - p1) / 2) ** 2 + np.cos(p1) * np.cos(p2) * np.sin(dl / 2) ** 2
    return 2 * radius_km * np.arcsin(np.sqrt(np.clip(a, 0, 1)))


def _unit_xyz(lat, lon):
    la, lo = np.radians(lat), np.radians(lon)
    return np.column_stack([np.cos(la) * np.cos(lo), np.cos(la) * np.sin(lo), np.sin(la)])


# --------------------------------------------------------------------------------------------------------------------
# One decoded message as seen through Herbie
# --------------------------------------------------------------------------------------------------------------------
@dataclass
class RefField:
    source: str  # original file path
    message_number: int  # 1-based, position in the source file
    ni: int
    nj: int
    alt_row_scanning: bool
    lat: np.ndarray  # (nj, ni) degrees north, canonical order (j = 0 is the first row, i increases east)
    lon: np.ndarray  # (nj, ni) degrees east wrapped to [-180, 180)
    values: np.ndarray  # (nj, ni) float64, NaN where the bitmap masks a cell
    reference_time: pd.Timestamp
    valid_time: pd.Timestamp
    herbie_var: str
    herbie_dims: tuple
    via: str  # how Herbie was used to read it
    row_fix_applied: bool  # True when cfgrib handed back mirrored odd rows and they were repaired here
    raw_bytes: bytes = field(repr=False, default=b"")
    _tree: object = field(repr=False, default=None)

    # -- derived -------------------------------------------------------------------------------------------------
    @property
    def n_cells(self) -> int:
        return self.ni * self.nj

    @property
    def has_mask(self) -> bool:
        return bool(np.isnan(self.values).any())

    def flat(self, i: int, j: int) -> int:
        return j * self.ni + i

    def cell_spacing_km(self, i: int, j: int) -> float:
        """Distance from cell (i, j) to its nearest in-grid neighbour along i or j."""
        ds = []
        for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            ii, jj = i + di, j + dj
            if 0 <= ii < self.ni and 0 <= jj < self.nj:
                ds.append(float(haversine_km(self.lat[j, i], self.lon[j, i], self.lat[jj, ii], self.lon[jj, ii])))
        return min(ds)

    @property
    def typical_spacing_km(self) -> float:
        return self.cell_spacing_km(self.ni // 2, self.nj // 2)

    # -- nearest cell (3-D KD-tree on the unit sphere, then great-circle distance) --------------------------------
    def _kdtree(self):
        if self._tree is None:
            from scipy.spatial import cKDTree

            self._tree = cKDTree(_unit_xyz(self.lat.ravel(), self.lon.ravel()))
        return self._tree

    def nearest(self, lat, lon):
        """Vectorised nearest cell. Returns (i, j, distance_km) arrays. lon may use either convention."""
        lat = np.atleast_1d(np.asarray(lat, dtype=np.float64))
        lon = np.atleast_1d(np.asarray(lon, dtype=np.float64))
        _, idx = self._kdtree().query(_unit_xyz(lat, lon))
        j, i = np.divmod(idx, self.ni)
        dist = haversine_km(lat, lon, self.lat[j, i], self.lon[j, i])
        return i, j, dist

    def classify_point(self, lat, lon):
        """'inside' (certainly in the grid footprint), 'outside' (certainly not) or 'ambiguous' (near the edge).

        A point in the footprint is within about 0.71 cell spacings of its nearest cell centre; a point more than one
        spacing from every centre is outside.  The band in between depends on the projection and is left unjudged.
        """
        i, j, d = self.nearest(lat, lon)
        out = []
        for ii, jj, dd in zip(i, j, d):
            s = self.cell_spacing_km(int(ii), int(jj))
            out.append("inside" if dd <= 0.49 * s else "outside" if dd >= 1.0 * s else "ambiguous")
        return out

    # -- boxes ---------------------------------------------------------------------------------------------------
    def box_mask(self, lon_min, lat_min, lon_max, lat_max, eps: float = 0.0) -> np.ndarray:
        """Boolean (nj, ni) mask of cells whose centre is inside the box, edges inclusive.

        ``eps`` (degrees) builds tolerance bands for comparing against float arithmetic done another way:
        eps > 0 grows the box (a superset), eps < 0 shrinks it (a subset).  Cells in the extension's answer must
        lie between the shrunk and the grown mask.
        """
        lat_ok = (self.lat >= lat_min - eps) & (self.lat <= lat_max + eps)
        span = lon_max - lon_min
        if span < 0:
            span += 360.0
        span = min(span, 360.0)
        if span >= 360.0:
            return lat_ok
        east = (self.lon - lon_min) % 360.0  # degrees east of lon_min, in [0, 360)
        if eps > 0:
            lon_ok = (east <= span + eps) | (east >= 360.0 - eps)
        elif eps < 0:
            lon_ok = (east >= -eps) & (east <= span + eps)
        else:
            lon_ok = east <= span
        return lat_ok & lon_ok


# --------------------------------------------------------------------------------------------------------------------
# Building a RefField through Herbie
# --------------------------------------------------------------------------------------------------------------------
def _grid_dims_and_alt(msg: bytes):
    h = ec.codes_new_from_message(msg)
    try:
        ni, nj = ec.codes_get(h, "Ni"), ec.codes_get(h, "Nj")
        alt = bool(ec.codes_get(h, "alternativeRowScanning"))
        date, hhmm = ec.codes_get(h, "dataDate"), ec.codes_get(h, "dataTime")
        ref = pd.Timestamp(f"{date:08d}") + pd.Timedelta(hours=hhmm // 100, minutes=hhmm % 100)
        return ni, nj, alt, ref
    finally:
        ec.codes_release(h)


def _ecc_nearest_value(msg: bytes, lat: float, lon: float) -> tuple[int, float]:
    h = ec.codes_new_from_message(msg)
    try:
        r = ec.codes_grib_find_nearest(h, lat, lon)[0]
        return int(r.index), float(r.value)
    finally:
        ec.codes_release(h)


def open_with_herbie(msg: bytes, workdir: Path, reference_time: pd.Timestamp):
    """Place ``msg`` where Herbie's NBM template expects a local file and open it with ``Herbie.xarray()``.

    A one-line local .idx is written next to it so Herbie never goes looking for one on a remote server.
    """
    from herbie import Herbie

    d = workdir / "nbm" / f"{reference_time:%Y%m%d}"
    d.mkdir(parents=True, exist_ok=True)
    name = f"blend.t{reference_time:%H}z.core.f001.co.grib2"
    (d / name).write_bytes(msg)
    (d / (name + ".idx")).write_text(f"1:0:d={reference_time:%Y%m%d%H}:VAR:level:1 hour fcst:\n")
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        H = Herbie(reference_time, model="nbm", product="co", fxx=1, save_dir=workdir, verbose=False)
        assert H.grib_source == "local", "Herbie went looking for the file remotely"
        try:
            ds, via = H.xarray(remove_grib=False), "Herbie.xarray"
        except NotImplementedError:
            # Herbie.xarray() attaches CF grid-mapping info and only knows Lambert and polar stereographic, so it
            # refuses Mercator grids (Hawaii, Puerto Rico, Guam).  Fall back to exactly what it does to read the
            # data: cfgrib with Herbie's backend arguments, minus the CRS step.
            import cfgrib

            ds = cfgrib.open_datasets(
                H.get_localFilePath(),
                backend_kwargs={"indexpath": "", "errors": "raise"},
                decode_timedelta=True,
            )
            via = "Herbie (locate) + cfgrib"
    if isinstance(ds, list):
        if len(ds) != 1:
            raise AssertionError(f"cfgrib returned {len(ds)} datasets for one message")
        ds = ds[0]
    return ds, via


def build_ref_field(source: Path, message_number: int, msg: bytes, workdir: Path) -> RefField:
    ni, nj, alt, ref_time = _grid_dims_and_alt(msg)
    ds, via = open_with_herbie(msg, workdir, ref_time)
    var_name = [v for v in ds.data_vars][0]
    var = ds[var_name]
    vals = np.asarray(var.values, dtype=np.float64).reshape(nj, ni)
    lat = np.asarray(ds["latitude"].values, dtype=np.float64).reshape(nj, ni)
    lon = wrap180(np.asarray(ds["longitude"].values, dtype=np.float64).reshape(nj, ni))

    # Does cfgrib's value order agree with ecCodes' own nearest-point answer on odd rows? If not, repair the field.
    fix = False
    if alt and nj > 2:
        probes = [(1, 3), (ni // 2, 1), (ni // 3, nj - 2 if (nj - 2) % 2 else nj - 3)]
        agree = 0
        usable = 0
        for pi, pj in probes:
            if pj % 2 == 0 or np.isnan(vals[pj, pi]):
                continue
            usable += 1
            _, ecc_val = _ecc_nearest_value(msg, float(lat[pj, pi]), float(lon[pj, pi]))
            agree += abs(ecc_val - vals[pj, pi]) <= 1e-4 * max(1.0, abs(ecc_val))
        if usable and agree < usable:
            fixed = vals.copy()
            fixed[1::2] = fixed[1::2, ::-1]
            fix_agree = 0
            for pi, pj in probes:
                if pj % 2 == 0 or np.isnan(fixed[pj, pi]):
                    continue
                _, ecc_val = _ecc_nearest_value(msg, float(lat[pj, pi]), float(lon[pj, pi]))
                fix_agree += abs(ecc_val - fixed[pj, pi]) <= 1e-4 * max(1.0, abs(ecc_val))
            if fix_agree == usable:
                vals, fix = fixed, True
            # else: neither order agrees; leave as is and let test_reference_sanity report it loudly

    return RefField(
        source=str(source),
        message_number=message_number,
        ni=ni,
        nj=nj,
        alt_row_scanning=alt,
        lat=lat,
        lon=lon,
        values=vals,
        reference_time=ref_time,
        valid_time=pd.Timestamp(ds["valid_time"].values),
        herbie_var=var_name,
        herbie_dims=tuple(var.dims),
        via=via,
        row_fix_applied=fix,
        raw_bytes=msg,
    )


class RefCache:
    """Session-wide cache so each message is decoded through Herbie once."""

    def __init__(self):
        self._tmp = Path(tempfile.mkdtemp(prefix="grib2_herbie_"))
        self._fields: dict[tuple[str, int], RefField] = {}
        self._n = 0

    def get(self, path: str | Path, message_number: int = 1) -> RefField:
        path = Path(path)
        key = (str(path.resolve()), message_number)
        if key not in self._fields:
            data = path.read_bytes()
            spans = split_messages(data)
            off, length = spans[message_number - 1]
            self._n += 1
            work = self._tmp / f"m{self._n}"
            self._fields[key] = build_ref_field(path, message_number, data[off : off + length], work)
        return self._fields[key]

    def close(self):
        shutil.rmtree(self._tmp, ignore_errors=True)


# --------------------------------------------------------------------------------------------------------------------
# The catalogue of messages the extension can read (regular lat/lon sample.grib2 is covered by the error tests)
# --------------------------------------------------------------------------------------------------------------------
FIXTURE_DIR = REPO / "fixtures"
DATA_DIR = REPO / "test" / "data"

GRIDS = {
    # name: (path, message_number, projection)
    "co": (FIXTURE_DIR / "nbm_msg.grib2", 1, "lambert"),
    "co_complex": (FIXTURE_DIR / "nbm_complex_msg.grib2", 1, "lambert"),  # packing template 5.2, categorical
    "ak": (FIXTURE_DIR / "nbm_ak_msg.grib2", 1, "polar_stereographic"),
    "hi": (FIXTURE_DIR / "nbm_hi_msg.grib2", 1, "mercator"),
    "pr": (FIXTURE_DIR / "nbm_pr_msg.grib2", 1, "mercator"),
    "gu": (FIXTURE_DIR / "nbm_gu_msg.grib2", 1, "mercator"),
    "gu_masked": (DATA_DIR / "masked_gu.grib2", 1, "mercator"),  # 400 cells masked out by a bitmap
}
# multi.grib2: message 1 = Guam, 2 = Puerto Rico, 3 = Hawaii, 4 = sample.grib2 (regular_ll, unsupported by read_grib2)
MULTI = DATA_DIR / "multi.grib2"
MULTI_IDX = DATA_DIR / "multi.grib2.idx"
