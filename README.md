# duckdb_grib2

A DuckDB extension for querying GRIB2 weather data with SQL, powered by [ecCodes](https://github.com/ecmwf/eccodes).
The end goal is to run queries directly against GRIB2 files in object storage, such as the
[NOAA National Blend of Models (NBM)](https://registry.opendata.aws/noaa-nbm/) open dataset on S3.

> **Status: early development.** The extension builds in CI with ecCodes linked and can decode real NOAA NBM messages, but the table functions below are planned and not yet available. `INSTALL grib2 FROM community` will work only after a community-extensions submission.

Built from the [DuckDB extension template](https://github.com/duckdb/extension-template).

## Planned API

Spatial selection comes first: most users want a few places, not a whole domain. See [docs/DESIGN.md](docs/DESIGN.md)
for the reasoning and measurements.

```sql
INSTALL grib2 FROM community;
LOAD grib2;

-- Catalog of messages in one or more files (one row per GRIB message)
SELECT * FROM grib2_index('s3://noaa-nbm-grib2-pds/blend.20260101/00/core/blend.t00z.core.f001.co.grib2');

-- Nearest grid cell to one or more points, for every matching message
SELECT valid_time, query_lat, query_lon, lat, lon, distance_km, value
FROM read_grib2('s3://noaa-nbm-grib2-pds/blend.20260101/00/core/*.grib2',
                search := 'TMP:2 m above ground',
                points := [{'lat': 37.21, 'lon': -93.29}]);

-- Every cell inside a box (lon_min, lat_min, lon_max, lat_max)
SELECT valid_time, lat, lon, value
FROM read_grib2('s3://noaa-nbm-grib2-pds/blend.20260101/00/core/blend.t00z.core.f001.co.grib2',
                search := 'TMP:2 m above ground',
                bbox := [-95.0, 36.0, -92.0, 38.0]);
```

These signatures are a design sketch and may change.

## Available today

`grib2_index` and `read_grib2` are the real table functions. `grib2_message_info` and `grib2_point_value` are temporary scaffolding:

```sql
SELECT grib2_eccodes_version();  -- version of the linked ecCodes library

-- One row per GRIB message. Uses the wgrib2-style .idx next to the file when there is one
SELECT message_number, byte_offset, byte_length, variable, level, forecast
FROM grib2_index('test/data/multi.grib2');

-- Decoded values for the cells you ask for: one row per cell per message, in long format
SELECT valid_time, variable, lat, lon, value, distance_km
FROM read_grib2('fixtures/nbm_msg.grib2', points := [{'lat': 37.21, 'lon': -93.29}]);           -- nearest cell
SELECT lat, lon, value
FROM read_grib2('test/data/masked_gu.grib2', bbox := [145.09, 14.65, 145.61, 15.15]);           -- lon_min, lat_min, lon_max, lat_max
-- read_grib2 also takes message := 133 (or a list) and search := 'TMP:2 m above ground' (a regular expression over
-- the .idx text), and needs one of points, bbox or all_cells := true.

-- Without the sidecar (or with idx := false) grib2_index hops through the file; details := true adds ecCodes names and grid info
SELECT message_number, short_name, grid_type, ni, nj, packing_type
FROM grib2_index('fixtures/nbm_msg.grib2', details := true);

-- Decode the first GRIB message in a blob and describe it
SELECT grib2_message_info(content) FROM read_blob('fixtures/nbm_msg.grib2');

-- Value at the grid cell nearest a point (NULL outside the grid)
SELECT grib2_point_value(content, 37.2090, -93.2923) FROM read_blob('fixtures/nbm_msg.grib2');  -- 291.5
```

For the NBM CONUS 2 m temperature message in `fixtures/`, `grib2_message_info` returns:

```
edition=2;shortName=2t;typeOfLevel=heightAboveGround;level=2;gridType=lambert;Ni=2345;Nj=1597;numberOfPoints=3744965;packingType=grid_complex_spatial_differencing;alternativeRowScanning=1;values=3744965;min=262.100;max=307.700
```

`grib2_index(path, idx := true, details := false)` accepts a glob and reads through DuckDB's file system, so `s3://` paths should work once `httpfs` is loaded (not tested yet). See [docs/DESIGN.md](docs/DESIGN.md) for the columns.

What the decode spike established: ecCodes definitions are available at runtime, and NBM's packing template (complex packing with spatial differencing) decodes with no extra codec libraries. A single CONUS message is about 3.7 million grid points, so spatial selection matters a lot. `grib2_point_value` supports every NBM grid (Lambert conformal for CONUS, polar stereographic for Alaska, Mercator for Hawaii, Puerto Rico and Guam), and decodes the whole message on every call, so it is for tests and spot checks.

**A trap worth knowing about:** this grid has `alternativeRowScanning = 1`, so ecCodes returns `values` with every other row stored east to west while latitudes and longitudes are in canonical order. Pairing them directly mirrors half the rows. See [docs/DESIGN.md](docs/DESIGN.md).

## Remote files

Both functions read through DuckDB's file system, so with `httpfs` loaded they work on `https://` URLs. Verified against the
public NOAA NBM bucket (macOS arm64, DuckDB v2.0.0-dev build):

```sql
LOAD httpfs;   -- INSTALL httpfs first on a released DuckDB
LOAD grib2;

-- The inventory of a 156 MB file: all 154 messages in 0.4 s (it reads only the .idx)
SELECT count(*), max(byte_offset + byte_length)
FROM grib2_index('https://noaa-nbm-grib2-pds.s3.amazonaws.com/blend.20261023/00/core/blend.t00z.core.f001.co.grib2');

-- 2 m temperature at one point: 0.8 s, reading the .idx and a single 2 MB range, not the whole file
SELECT valid_time, variable, round(value, 1)
FROM read_grib2('https://noaa-nbm-grib2-pds.s3.amazonaws.com/blend.20261023/00/core/blend.t00z.core.f001.co.grib2',
                search := '^TMP:2 m above ground:1 hour fcst$',
                points := [{'lat': 37.209, 'lon': -93.2923}]);
```

Not yet tried: the `s3://` scheme, globs over a bucket, and a remote file with no `.idx` (the existence check may behave
differently on a 403).

**`httpfs` in a locally built binary.** A binary built from the `duckdb` submodule has no published `httpfs`, so
`INSTALL httpfs` returns a 404. Build it in by adding this to your local `extension_config.cmake` (do not commit it) and
running `make` again:

```cmake
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG a2485c0fd240cbddc0d467019f30201c9a46cf06
)
```

Then load both by path (`httpfs` first):

```sh
./build/release/duckdb -unsigned \
  -c "LOAD './build/release/extension/httpfs/httpfs.duckdb_extension';" \
  -c "LOAD './build/release/extension/grib2/grib2.duckdb_extension';"
```

## Performance

Measured on an 8-core Apple silicon Mac with the real NBM CONUS file, local disk:

| Query | Time |
|---|---|
| `read_grib2` points query across all 154 messages | 3.2 s (about 20 ms per message, one thread) |
| Every cell of one message (3.7M rows, aggregated) | 0.3 s |

A spatial filter does not make decoding cheaper: a message is decoded in full whatever you select, so filters save rows and
memory rather than decode time. Scans are single-threaded for now.

## Roadmap

- [x] Extension scaffold with ecCodes linked via vcpkg, building in CI
- [x] Decode spike: decode a real NBM message from an in-memory buffer
- [x] Grid geometry for all NBM projections (Lambert, polar stereographic, Mercator): nearest cell, lat/lon box, value ordering, tested against ecCodes for every region
- [x] Point extraction from a message inside the extension (`grib2_point_value`)
- [x] Survey the other NBM regions' grids (all use the same packing and stored-row order; three projections)
- [x] `.idx` parser and GRIB2 section-header parser (standalone, tested against the real NBM inventory and ecCodes)
- [x] `grib2_index()` table function on local files (`.idx` sidecar, header hopping, optional ecCodes details)
- [x] Packing survey across the real NBM f001 files (only templates 5.2 and 5.3)
- [x] `grib2_index()` and `read_grib2()` over HTTPS through `httpfs`, against the public NBM bucket
- [ ] `s3://` URLs, globs over a bucket, a remote file with no `.idx`, and time series across several files
- [x] `read_grib2()` with `message`, `search`, `points`, `bbox` and `all_cells` on local files, single-threaded
- [ ] Filter and projection pushdown
- [ ] Parallel scans, S3 reads, community extension submission

## Building
### Managing dependencies
DuckDB extensions use VCPKG for dependency management (this extension depends on ecCodes). Follow the [installation instructions](https://vcpkg.io/en/getting-started) or run the following:
```shell
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

### Build steps
Clone with submodules (`git clone --recurse-submodules`, or `git submodule update --init --recursive`), then run:
```sh
make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/grib2/grib2.duckdb_extension
```
- `duckdb` is the DuckDB shell built alongside the extension. It does not load the extension automatically; see "Running the extension" below.
- `unittest` is the test runner of DuckDB, with the extension linked in.
- `grib2.duckdb_extension` is the loadable binary as it would be distributed.

### Build notes

- **`python3` must be on your `PATH`** for the ecCodes build (see the overlay port below).
- **ecCodes overlay port.** `vcpkg_ports/eccodes/` is a copy of the upstream vcpkg `eccodes` 2.47.0 port (from the vcpkg commit pinned by CI) with one change: the host `python3` dependency is removed. That dependency pulls in `python3[extensions]` and `libb2` on non-Windows platforms, and `libb2` needs `autoconf-archive` and `automake`, which the Linux CI images do not provide. The portfile still finds a system `python3` through `vcpkg_find_acquire_program`. When bumping the pinned vcpkg commit, refresh this copy from upstream and reapply the change.
- **Overlay registration.** `vcpkg.json` lists `./vcpkg_ports` ahead of `./extension-ci-tools/vcpkg_ports` under `overlay-ports`.

## Platform support

The extension builds on the default CI platforms, including Windows with MSVC (`windows_amd64`). These are excluded in `.github/workflows/MainDistributionPipeline.yml` (`exclude_archs`):

| Platform | Reason |
|---|---|
| `wasm_mvp`, `wasm_eh`, `wasm_threads` | ecCodes does not configure on the community `wasm32-emscripten` vcpkg triplet, and wasm is not a realistic target for this use case |
| `windows_amd64_mingw` | ecCodes fails to configure on `x64-mingw-static`; to be investigated |

These exclusions will need to be mirrored in the `description.yml` for the community-extensions submission.

## CI notes

- The code-quality job runs `format` only (`format_checks: 'format'`). The `tidy` check is disabled because it configures the project without vcpkg, so `find_package(eccodes)` cannot succeed there.
- A separate workflow (`.github/workflows/GridCheck.yml`) builds and runs the standalone C++ checks (grid geometry, `.idx` and section parsing) on every push.
- Run `make format-check` or `make format-fix` locally before pushing.

## Running the extension

A locally built extension is unsigned, so start the shell with `-unsigned` and load the binary by path (run from the repo root):

```sh
./build/release/duckdb -unsigned \
  -c "LOAD './build/release/extension/grib2/grib2.duckdb_extension';" \
  -c "SELECT grib2_message_info(content) FROM read_blob('fixtures/nbm_msg.grib2');"
```

Or interactively:

```sql
LOAD './build/release/extension/grib2/grib2.duckdb_extension';
SELECT grib2_eccodes_version();
```

## Running the tests
SQL tests live in `./test/sql` and run with:
```sh
make test
```
`make test` calls DuckDB's `run_tests.py`, which needs **Python 3.10 or newer**. On older Pythons (for example macOS's system Python 3.9) run the test binary directly instead:
```sh
./build/release/test/unittest "test/*"
```
Tests read their data files relative to the repository root, so run them from there. Test data is described in `fixtures/README.md` and `test/data/`.

The grid geometry and the `.idx` and section parsers have their own standalone checks (no DuckDB or ecCodes needed), also run in CI:
```sh
g++ -std=c++17 -O2 -I src/include test/cpp/grid_check.cpp -o /tmp/grid_check && /tmp/grid_check
g++ -std=c++17 -O2 -I src/include test/cpp/idx_check.cpp -o /tmp/idx_check && /tmp/idx_check
```

## Installing

There is no published release yet. Until the extension is submitted to the community-extensions repository, build it locally and load the binary by path as shown above. Once published, installation will be:

```sql
INSTALL grib2 FROM community;
LOAD grib2;
```

(This repository does not currently run the template's S3 deploy job, so the template's `custom_extension_repository` instructions do not apply.)

