"""Runs SQL against the compiled grib2 extension and hands back pandas DataFrames.

Two back ends, chosen automatically (override with the environment variables below):

1. The DuckDB CLI built next to the extension (``build/release/duckdb``).  Preferred: it is guaranteed to be the same
   DuckDB version the extension was compiled against.  Large results go through a temporary CSV file.
2. The ``duckdb`` Python package, loading the extension by path.  This only works when the package's version matches
   the DuckDB the extension was built against (a loadable extension is tied to one exact version).

Environment variables:
    GRIB2_DUCKDB_BIN    path to a DuckDB CLI  (default: <repo>/build/release/duckdb)
    GRIB2_EXTENSION     path to grib2.duckdb_extension
                        (default: <repo>/build/release/extension/grib2/grib2.duckdb_extension)
    GRIB2_HTTPFS        optional path to httpfs.duckdb_extension, loaded first (only for the network tests)
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

import pandas as pd

REPO = Path(__file__).resolve().parents[2]
NULL_TOKEN = "__NULL__"


class Grib2Unavailable(Exception):
    """The extension binary could not be found or loaded."""


def sql_str(text: str) -> str:
    return "'" + str(text).replace("'", "''") + "'"


def _num(x) -> str:
    return format(float(x), ".17g")


def points_literal(points) -> str:
    return "[" + ", ".join("{'lat': %s, 'lon': %s}" % (_num(la), _num(lo)) for la, lo in points) + "]"


def read_grib2_sql(
    path,
    *,
    message=None,
    search=None,
    points=None,
    bbox=None,
    all_cells=False,
    select="*",
    where=None,
    order_by=None,
    flat_mod=None,
) -> str:
    args = [sql_str(path)]
    if message is not None:
        args.append("message := " + (str(list(message)) if isinstance(message, (list, tuple)) else str(int(message))))
    if search is not None:
        args.append("search := " + sql_str(search))
    if points is not None:
        args.append("points := " + points_literal(points))
    if bbox is not None:
        args.append("bbox := [" + ", ".join(_num(b) for b in bbox) + "]")
    if all_cells:
        args.append("all_cells := true")
    q = f"SELECT {select} FROM read_grib2({', '.join(args)})"
    if flat_mod:  # (ni, modulus, remainder): keep every cell whose flat index j * ni + i has that remainder
        ni, mod, rem = flat_mod
        extra = f"(j * {int(ni)} + i) % {int(mod)} = {int(rem)}"
        where = f"({where}) AND {extra}" if where else extra
    if where:
        q += f" WHERE {where}"
    if order_by:
        q += f" ORDER BY {order_by}"
    return q


class Grib2Client:
    """Thin facade used by the tests. ``is_oracle`` is False here; see oracle.py for the self-check stand-in."""

    is_oracle = False

    def __init__(self, extension: Path, cli: Path | None, httpfs: Path | None = None):
        self.extension = Path(extension)
        self.cli = Path(cli) if cli else None
        self.httpfs = Path(httpfs) if httpfs else None
        self._con = None
        if self.cli is None:
            import duckdb

            self._con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
            try:
                if self.httpfs:
                    self._con.execute(f"LOAD {sql_str(self.httpfs)}")
                self._con.execute(f"LOAD {sql_str(self.extension)}")
            except Exception as exc:  # version mismatch is the usual reason
                raise Grib2Unavailable(
                    f"could not load {self.extension} into python duckdb {duckdb.__version__}: {exc}\n"
                    "Build the CLI (make) and set GRIB2_DUCKDB_BIN, or install the matching duckdb version."
                ) from exc

    # -- discovery ---------------------------------------------------------------------------------------------------
    @classmethod
    def discover(cls) -> "Grib2Client":
        ext = Path(os.environ.get("GRIB2_EXTENSION", REPO / "build/release/extension/grib2/grib2.duckdb_extension"))
        cli_env = os.environ.get("GRIB2_DUCKDB_BIN")
        cli = Path(cli_env) if cli_env else REPO / "build/release/duckdb"
        httpfs = os.environ.get("GRIB2_HTTPFS")
        if not ext.exists():
            raise Grib2Unavailable(
                f"extension not found at {ext}. Build it with `make` or set GRIB2_EXTENSION=/path/to/grib2.duckdb_extension"
            )
        client = cls(ext, cli if cli.exists() else None, Path(httpfs) if httpfs else None)
        client.sql("SELECT grib2_eccodes_version()")  # fail early if the extension does not load
        return client

    # -- execution ---------------------------------------------------------------------------------------------------
    def _cli_args(self, *stmts: str) -> list[str]:
        args = [str(self.cli), "-unsigned", "-c", "SET threads=2"]
        if self.httpfs:
            args += ["-c", f"LOAD {sql_str(self.httpfs)}"]
        args += ["-c", f"LOAD {sql_str(self.extension)}"]
        for s in stmts:
            args += ["-c", s]
        return args

    def sql(self, query: str) -> pd.DataFrame:
        """Run a query and return all rows. Raises RuntimeError with DuckDB's message on failure."""
        if self._con is not None:
            try:
                return self._con.execute(query).df()
            except Exception as exc:
                raise RuntimeError(str(exc)) from exc
        with tempfile.TemporaryDirectory() as td:
            out = Path(td) / "result.csv"
            copy = f"COPY ({query}) TO {sql_str(out)} (FORMAT csv, HEADER true, NULLSTR {sql_str(NULL_TOKEN)})"
            proc = subprocess.run(self._cli_args(copy), capture_output=True, text=True, cwd=REPO)
            if proc.returncode != 0:
                if proc.returncode < 0 or "Segmentation" in proc.stderr:
                    raise AssertionError(
                        f"DuckDB CLI CRASHED (rc={proc.returncode}) on: {query}\n{proc.stderr[-2000:]}"
                    )
                raise RuntimeError((proc.stderr or proc.stdout).strip())
            df = pd.read_csv(out, na_values=[NULL_TOKEN], keep_default_na=False, low_memory=False)
            return df

    def error(self, query: str) -> str | None:
        """Return the error message if the query fails (gracefully), None if it succeeds. A crash raises."""
        try:
            self.sql(query)
            return None
        except RuntimeError as exc:
            return str(exc)

    # -- convenience wrappers around the two table functions -----------------------------------------------------------
    def read(self, path, **kwargs) -> pd.DataFrame:
        df = self.sql(read_grib2_sql(path, **kwargs))
        for col in ("reference_time", "valid_time"):
            if col in df:
                df[col] = pd.to_datetime(df[col])
        return df

    def index(self, path, **kwargs) -> pd.DataFrame:
        extra = "".join(f", {k} := {str(v).lower()}" for k, v in kwargs.items())
        return self.sql(f"SELECT * FROM grib2_index({sql_str(path)}{extra}) ORDER BY file_path, message_number")
