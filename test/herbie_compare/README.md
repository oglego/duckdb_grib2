# DuckDB vs Herbie comparison suite

Checks that `read_grib2` / `grib2_index` / the scalar helpers return what **Herbie** (via cfgrib/ecCodes) reports for
the same GRIB2 bytes. Herbie is pointed at local copies of the repo fixtures, so no network is needed.

## Run

The Python environment is managed with [uv](https://docs.astral.sh/uv/) (`pyproject.toml` and `uv.lock` live in this
directory). The extension itself is built separately with `make`.

```bash
make                                   # from the repo root: builds build/release/duckdb and the extension

cd test/herbie_compare
uv sync                                # creates .venv here from uv.lock (first run only, or after dependency changes)
uv run pytest                          # default run
uv run pytest --runslow                # also every cell of the 3.7M (CONUS) and 1.8M (Alaska) grids
```

Handy variations:

```bash
uv run pytest test_points.py                      # one module
uv run pytest -k "hi or gu"                       # only tests whose name matches
uv run pytest -x -q                               # stop at the first failure, terse output
uv run pytest test_reference_sanity.py            # needs no extension: checks the Herbie-derived reference only

# from the repo root, without changing directory
uv run --project test/herbie_compare pytest test/herbie_compare

# point at a build in another location
GRIB2_DUCKDB_BIN=/path/to/duckdb GRIB2_EXTENSION=/path/to/grib2.duckdb_extension uv run pytest
```

Managing dependencies (run in this directory; commit the updated `uv.lock`):

```bash
uv add <package>                       # add a dependency
uv remove <package>                    # remove one
uv lock --upgrade                      # refresh locked versions
uv sync --frozen                       # install exactly what uv.lock says (useful in CI)
```

`uv` creates `.venv/` with its own `.gitignore`, so it is not committed. `requirements.txt` is kept only as a plain-pip
fallback (`pip install -r requirements.txt`); `pyproject.toml` is the source of truth.

The extension is found at `build/release/extension/grib2/grib2.duckdb_extension` and queried through
`build/release/duckdb` (same DuckDB version as the extension). Override with `GRIB2_EXTENSION` and
`GRIB2_DUCKDB_BIN`. Without the CLI, the Python `duckdb` package is used, but its version must match the build.
If no extension is found, everything except `test_reference_sanity.py` is skipped.

## What is covered

| File | Covers |
|---|---|
| `test_reference_sanity.py` | The Herbie-derived reference itself, against ecCodes and brute force (no extension needed) |
| `test_points.py` | Nearest cell, value, distance; cell centres; ties; grid corners/edges; just inside/outside the footprint; 0..360 vs -180..180; Alaska across the antimeridian; poles; duplicates and ordering; integer coordinates; masked cells; categorical (template 5.2) values; alternate-row-scan values |
| `test_bbox.py` | Boxes of many sizes on every grid; empty/partial/world boxes; both longitude conventions; antimeridian and polar boxes; inclusive edges; ordering; masked cells |
| `test_all_cells.py` | Every cell's lat/lon/value on the smaller grids; strided samples and aggregates on the large ones |
| `test_metadata.py` | `grib2_index` vs Herbie's inventory of the real 154-message `.idx`; message selection; `search` vs a regex over Herbie's inventory; globs; `grib2_point_value`, `grib2_message_info` |
| `test_errors.py` | Bad arguments, NaN/inf, unsupported grids, stale `.idx`, truncated/corrupt/garbage files, header hopping |

## Things worth knowing about Herbie as an oracle

* `Herbie.xarray()` raises `NotImplementedError` for Mercator grids (Hawaii, Puerto Rico, Guam). For those, the
  reference uses Herbie to locate the file and cfgrib with Herbie's arguments to read it (`RefField.via`).
* cfgrib returns **mirrored odd rows** for grids it exposes as a flat `values` dimension (the same Mercator grids),
  because these messages use alternative row scanning. `reference.py` detects this against ecCodes and repairs it;
  `test_reference_sanity.py` verifies the result. Comparing raw Herbie output to the extension would report false
  failures on those grids.

## Checking the suite itself

`GRIB2_ORACLE=1 uv run pytest` runs the spatial tests against a stand-in built from the reference
(`oracle.py`); all should pass. `GRIB2_ORACLE_BUG=<name>` injects a bug (`mirror_odd_rows`, `off_by_one_i`,
`no_lon_wrap`, `open_bbox_edges`, `drop_masked`, `unsorted_bbox`) and the suite must then fail.

## Limits

* `valid_time` is reference time + the step from the `.idx`, so it is NULL for files without a sidecar. The tests assert
  NULL there and, when a sidecar gives the step, equality with Herbie's `valid_time`.

* "Inside the grid" is the extension's half-cell footprint rule. The tests assert behaviour only where it is
  unambiguous (within 0.49 spacings of a centre = inside, more than 1 spacing = outside).
* Bbox comparisons allow 1e-7 degrees of float noise at the edges.
* No live-bucket tests are included; the fixtures are real NBM messages.
