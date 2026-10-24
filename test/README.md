# Testing this extension
This directory contains all the tests for this extension. The `sql` directory holds tests that are written as [SQLLogicTests](https://duckdb.org/dev/sqllogictest/intro.html). DuckDB aims to have most its tests in this format as SQL statements, so for the grib2 extension, this should probably be the goal too.

The root makefile contains targets to build and run all of these tests. To run the SQLLogicTests:
```bash
make test
```
or 
```bash
make test_debug
```

## Test data

- `test/data/sample.grib2`: a 277-byte synthetic GRIB2 message (16 x 31 grid, complex packing with spatial differencing). Regenerate it with `test/data/generate_sample.py` (needs `pip install eccodes ecmwflibs`).
- `fixtures/nbm_msg.grib2`: a real NBM CONUS message. See `fixtures/README.md` for its provenance.

Tests use paths relative to the repository root, so run them from there.

## Running tests

`make test` needs Python 3.10 or newer (it calls DuckDB's `run_tests.py`). Otherwise run the binary directly:
```bash
./build/release/test/unittest "test/*"
```
