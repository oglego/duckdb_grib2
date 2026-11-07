# DuckDB vs Herbie comparison suite

Checks that `read_grib2` / `grib2_index` / the scalar helpers return what **Herbie** (via cfgrib/ecCodes) reports for
the same GRIB2 bytes. Herbie is pointed at local copies of the repo fixtures, so no network is needed.

## Run

```bash
pip install -r test/herbie_compare/requirements.txt
make                                   # builds build/release/duckdb and the extension
pytest test/herbie_compare             # default run
pytest test/herbie_compare --runslow   # also every cell of the 3.7M (CONUS) and 1.8M (Alaska) grids
```

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

`GRIB2_ORACLE=1 pytest test/herbie_compare` runs the spatial tests against a stand-in built from the reference
(`oracle.py`); all should pass. `GRIB2_ORACLE_BUG=<name>` injects a bug (`mirror_odd_rows`, `off_by_one_i`,
`no_lon_wrap`, `open_bbox_edges`, `drop_masked`, `unsorted_bbox`) and the suite must then fail.

## Limits

* "Inside the grid" is the extension's half-cell footprint rule. The tests assert behaviour only where it is
  unambiguous (within 0.49 spacings of a centre = inside, more than 1 spacing = outside).
* Bbox comparisons allow 1e-7 degrees of float noise at the edges.
* No live-bucket tests are included; the fixtures are real NBM messages.
