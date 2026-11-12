# GRIB2 examples

These scripts demonstrate reading HRRR surface-analysis GRIB2 files from the
public NOAA S3 bucket with the DuckDB GRIB2 extension. 

## Current examples

- `compare_cells.py`: compares DuckDB `read_grib2(..., bbox := ...)` results to
  Herbie's cell selection logic and shows how many wide-table columns each choice
  would produce.
- `herbie_comparison.py`: compares DuckDB and Herbie outputs across a date range
  and asserts the selected cell keys and values match.
- `hrrr_to_wide.py`: builds a wide table of HRRR grid values for one or more days,
  reducing each day to a compact, query-friendly result set.
- `struct_pack.py`: groups grid cells into nested `struct_pack` records by valid
  time and writes the result to `duckdb_grib2.csv`.

## Requirements

- Python 3.12 or newer
- [uv](https://docs.astral.sh/uv/)
- Network access to the NOAA HRRR S3 bucket
- A release build of the extension whose version matches the pinned DuckDB
  Python package (`2.0.0`)

From the repository root, build the matching extension:

```sh
OVERRIDE_GIT_DESCRIBE=v2.0.0 make
```

The override matters: without it, a source build identifies as a development
version (for example, `v2.0.0-dev88614`) and cannot be loaded by the released
`2.0.0` Python package.

Then, from this directory, install the example dependencies:

```sh
uv sync
```

Run any script from this directory so its relative extension path resolves:

```sh
uv run compare_cells.py
uv run herbie_comparison.py
uv run hrrr_to_wide.py
uv run struct_pack.py
```

## `compare_cells.py`: inspect the bounding-box choices

This example runs the same HRRR selection through the DuckDB extension and
[Herbie](https://herbie.readthedocs.io/) and compares which grid cells are kept.
It shows the difference between:

- Herbie cells whose centres fall inside the extent,
- the wider rectangle of grid rows and columns that touch the extent, and
- DuckDB `read_grib2(..., bbox := ...)` output.

It prints how many wide-table columns each selection would create and highlights
which option matches the target shape.

## `herbie_comparison.py`: validate DuckDB results against Herbie

This example reads a date range and checks the returned grid-cell keys exactly,
then verifies latitude, longitude and each selected field with
`numpy.isclose` (`rtol=1e-6`, `atol=1e-4`). It raises an assertion if keys or
values differ; otherwise it prints a sample and confirms the results match.
The comparison focuses on correctness across the selected time range.

## `hrrr_to_wide.py`: build a compact wide table

This example reads a set of HRRR cycles, aggregates the selected variables to one
row per valid time and grid cell, and then builds a wide table for further
analysis in Python. It is designed around the idea of reading each day once,
keeping the raw data in DuckDB, and then restructuring it into a query-friendly
shape without re-fetching the GRIB files.

## `struct_pack.py`: group grid cells into nested records

This example uses DuckDB to transform the selected fields into a list of
`struct_pack` records for each valid time, with the grid cells ordered by
latitude and longitude. It prints a sample of the result and writes the query
output to `duckdb_grib2.csv` in the current directory.
