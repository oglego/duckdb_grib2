# grib2: GRIB2 weather data in DuckDB

A DuckDB extension for querying GRIB2 files with SQL. List the messages in a file, then read decoded values at the points
or inside the bounding box you care about, from local files or from object storage such as the
[NOAA National Blend of Models (NBM)](https://registry.opendata.aws/noaa-nbm/) bucket. Decoding is done by
[ecCodes](https://github.com/ecmwf/eccodes).

> **Status: early.** It works on local files and, through `httpfs`, on the public NBM bucket. It has been tested on NBM
> data (all five regions) and has not been published to the community-extensions repository yet, so there is no
> `INSTALL grib2 FROM community` yet; see [Installing](#installing).

## Quickstart

```sql
LOAD grib2;

-- What is in a GRIB2 file? Reads only the .idx that sits next to it, when there is one
SELECT message_number, variable, level, forecast FROM grib2_index('path/to/file.grib2') LIMIT 10;

-- 2 m temperature at a point, as a time series across forecast hours
SELECT valid_time, round(value, 1) AS kelvin
FROM read_grib2('blend.t00z.core.f00[1-6].co.grib2',
                search := '^TMP:2 m above ground:\d+ hour fcst$',
                points := [{'lat': 37.21, 'lon': -93.29}])
ORDER BY valid_time;

-- Every cell inside a box (lon_min, lat_min, lon_max, lat_max)
SELECT lat, lon, value FROM read_grib2('path/to/file.grib2', message := 133, bbox := [-95, 36, -92, 38]);
```

Local files need nothing else. For `https://` or `s3://` paths, load `httpfs` first (see [Remote files](#remote-files)).

## Functions

### `grib2_index(path, idx := true, details := false)`

One row per GRIB message: where it is and what it contains. `path` may be a glob. Rows come from the first of these that applies:

1. **The `.idx` sidecar** next to the file (`<path>.idx`, the wgrib2-style inventory NOAA publishes), when it exists and `idx`
   is true. One small read for the whole file.
2. **Header hopping** when there is no sidecar (or `idx := false`): a short read per message, using the length in each
   message's indicator section. No names, but parameter numbers, grid and packing templates and the reference time.
3. **`details := true`** additionally asks ecCodes for names, level and grid details, from a short prefix of each message.

| Columns | From |
|---|---|
| `file_path`, `message_number`, `byte_offset`, `byte_length` | the sidecar, or hopping |
| `reference_time` | the sidecar, or the message |
| `variable`, `level`, `forecast`, `qualifier`, `inventory` | the sidecar text; `inventory` is `variable:level:forecast[:qualifier]` |
| `step_hours`, `percentile`, `threshold_lower`, `threshold_upper` | parsed from the sidecar text |
| `discipline`, `parameter_category`, `parameter_number`, `grid_template`, `packing_template`, `packing_type` | the message headers (hopping or `details`) |
| `short_name`, `type_of_level`, `level_value`, `grid_type`, `ni`, `nj`, `alternative_row_scanning` | ecCodes (`details := true`) |

Columns that do not apply to the mode used are NULL. A sidecar that does not belong to the file is detected and reported
instead of giving wrong offsets.

### `read_grib2(path, message := ..., search := ..., points := ..., bbox := ..., all_cells := false)`

Decoded values in long format, one row per selected cell per message. `path` may be a glob.

**Which messages.** All of them by default. `message := 133` or `[133, 134]` selects by message number. `search := '...'`
is a regular expression (ECMAScript syntax) matched against the sidecar's inventory text, so it needs the `.idx`. The text
looks like `TMP:2 m above ground:1 hour fcst`; it is matched as it is and with colons around it, so
`':TMP:2 m above ground:'` selects whole fields, while an unanchored `TMP:2 m above ground` would also match
`APTMP:2 m above ground`. Use `^` and `$` to pin down a single message. `message` and `search` can be combined.

**Which cells.** Exactly one of:

| Parameter | Meaning |
|---|---|
| `points := [{'lat': 37.21, 'lon': -93.29}, ...]` | the nearest grid cell to each point |
| `bbox := [lon_min, lat_min, lon_max, lat_max]` | every cell whose centre is inside the box |
| `all_cells := true` | the whole grid (3.7 million rows per NBM CONUS message) |

With none of them the function raises an error, because one CONUS message has 3.7 million cells. Longitudes may be given as
0 to 360 or -180 to 180, and a box with `lon_max < lon_min` crosses the antimeridian.

**Output columns:** `file_path`, `message_number`, `reference_time`, `valid_time` (the reference time plus the step, NULL
when the forecast is not a plain `anl` or `N hour fcst`), `variable`, `level`, `forecast`, `qualifier` (from the
sidecar, NULL without one), `short_name` (ecCodes), `lat`, `lon`, `i`, `j`, `value`, and in `points` mode `query_lat`,
`query_lon` and `distance_km`, so results join back to a table of stations.

- Cells masked out by a bitmap have a NULL `value`.
- A point outside a message's grid produces no row for that message.
- Rows come back in a fixed order: files in path order, messages in file order, and within a message in the order of
  the points or in grid order.

### Helpers for messages held in a BLOB

These work on one GRIB message already in memory, for example from `read_blob()` or a byte range you fetched yourself.

```sql
SELECT grib2_eccodes_version();                                   -- the ecCodes version linked in
SELECT grib2_message_info(content) FROM read_blob('file.grib2');  -- first message: names, grid, packing, min and max
SELECT grib2_point_value(content, 37.21, -93.29) FROM read_blob('file.grib2');  -- value at the nearest cell, NULL outside
```

`grib2_point_value` decodes the whole message on every call, so use `read_grib2` for more than a spot check.

## Remote files

Both table functions read through DuckDB's file system, so with `httpfs` loaded they work on `https://` and `s3://` URLs.
Checked against the public NBM bucket:

```sql
INSTALL httpfs; LOAD httpfs;   -- once
LOAD grib2;
SET s3_region = 'us-east-1';   -- for s3:// URLs on the public bucket

-- The inventory of a 156 MB file, reading only its .idx
SELECT count(*), max(byte_offset + byte_length)
FROM grib2_index('https://noaa-nbm-grib2-pds.s3.amazonaws.com/blend.YYYYMMDD/00/core/blend.t00z.core.f001.co.grib2');

-- 2 m temperature at one point for six forecast hours: reads six .idx files and six 2 MB message ranges
SELECT valid_time, round(value, 1) AS kelvin
FROM read_grib2('s3://noaa-nbm-grib2-pds/blend.YYYYMMDD/00/core/blend.t00z.core.f00[1-6].co.grib2',
                search := '^TMP:2 m above ground:\d+ hour fcst$',
                points := [{'lat': 37.209, 'lon': -93.2923}])
ORDER BY valid_time;
```

Replace `YYYYMMDD` with a recent run date: the bucket keeps a limited window of recent runs.

Only the bytes you ask for are read: a point from one 156 MB file reads its `.idx` and one 2 MB range. Without a sidecar,
`grib2_index` makes one request per message, so use the `.idx` where there is one. `read_grib2` indexes several files at
once and fetches the next few messages while it decodes the current one.

## What it reads

- **Grids:** Lambert conformal (NBM CONUS), polar stereographic (north, NBM Alaska) and Mercator (NBM Hawaii, Puerto Rico,
  Guam), on a spherical earth, with the usual scanning mode (rows stored west to east and south to north, with or without
  alternating row direction). Other grid types (for example regular latitude/longitude, as in GFS) and ellipsoidal earths
  give a "not supported yet" error.
- **Packing:** ecCodes decodes the values. The packing templates exercised by the tests are 5.3 (complex with spatial
  differencing) and 5.2 (complex). Others are passed to ecCodes but not tested here. JPEG 2000 support (OpenJPEG) is built
  in, untested; CCSDS is not built in.
- **Inventory files:** the `.idx` format NOAA publishes (wgrib2 style). Lines of the form `n.m` (several fields in one
  message) are not supported yet.
- **Tested data:** NBM CONUS, Alaska, Hawaii, Puerto Rico and Guam, forecast hour 1 of the core product. Other sources, such
  as HRRR (also Lambert conformal), should work but have not been tried.

## Settings, limits and untrusted input

```sql
SET grib2_io_threads = 8;   -- default 8, range 1 to 64
```

`grib2_index` and `read_grib2` overlap network round trips by reading from a few background threads. They call DuckDB's
file system from those threads rather than from DuckDB's own. If a filesystem extension misbehaves under that, or the
environment restricts thread creation, `SET grib2_io_threads = 1` keeps every read on the query's thread (slower on
remote files, same results).

Limits, each reported as an error that names the file and message:

| Limit | Value |
|---|---|
| `.idx` file size | 64 MiB |
| One message | 1 GiB |
| Grid size (`Ni x Nj`) | 250 million cells |
| `search` pattern length | 1024 characters |
| Message bytes held in flight by prefetching | 128 MiB (at least one message) |

Grid definitions are validated before use: non-positive spacing, a non-finite coordinate or earth radius, or a grid over
the limit is refused instead of producing wrong or non-finite coordinates.

**Untrusted input.** The `.idx` and section-header parsers are covered by sanitizer and fuzz runs in CI. The decoding
itself is done by ecCodes, which parses the whole message and has had memory-safety bugs fixed in the past; it is not
fuzzed here. Read files you trust, or run DuckDB where a crash is contained, when the source of the GRIB2 data is not
under your control. A malformed message produces an error in most cases, but that is not a guarantee against a crash in
ecCodes.

Other things to know:

- URLs with a query string (for example presigned S3 URLs) are not given a sidecar lookup: `.idx` is appended to the
  whole string, so the inventory is not found and `search` is unavailable. Use `message := ...` there.
- With a bitmap, a cell whose decoded value is exactly the message's missing-value marker (9999 unless the message says
  otherwise) is returned as NULL.
- Errors from reading a file (network, permissions) keep their DuckDB error type and name the file and message number.
- `read_grib2` reads and decodes on one thread; query cancellation is honoured between messages.

## Performance

Measured on an 8-core Apple silicon Mac, from the author's network, on the real NBM CONUS file:

| Query | Time |
|---|---|
| `read_grib2` points query across all 154 messages, local file | 3.2 s (about 20 ms per message) |
| Every cell of one message (3.7M rows, aggregated), local file | 0.3 s |
| A point for six forecast hours over `s3://` (six files) | 1.4 s (4.3 s before overlapping requests) |
| The inventory of a 156 MB remote file | 0.4 s |
| Header hopping on a remote file without a sidecar | about 75 ms per message |

A message is decoded in full whatever you select, so spatial filters save rows and memory, not decode time. Decoding is
single-threaded; remote reads are overlapped.

## Installing

There is no published release yet. Until the extension is in the community-extensions repository, build it from source
and load the binary by path (see below). Once published:

```sql
INSTALL grib2 FROM community;
LOAD grib2;
```

## Building from source

DuckDB extensions use vcpkg for dependencies (this one needs ecCodes). Follow the
[vcpkg installation instructions](https://vcpkg.io/en/getting-started) or run:

```sh
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Clone with submodules (`git clone --recurse-submodules`, or `git submodule update --init --recursive`), then:

```sh
make
```

This builds `./build/release/duckdb` (a DuckDB shell), `./build/release/test/unittest` (the test runner) and
`./build/release/extension/grib2/grib2.duckdb_extension` (the loadable extension). A locally built extension is unsigned and
is not loaded automatically:

```sh
./build/release/duckdb -unsigned -c "LOAD './build/release/extension/grib2/grib2.duckdb_extension';" -c "SELECT grib2_eccodes_version();"
```

**Notes**

- `python3` must be on your `PATH` for the ecCodes build.
- `vcpkg_ports/eccodes/` is a copy of the upstream vcpkg `eccodes` 2.47.0 port (from the vcpkg commit the DuckDB CI uses)
  with one change: the host `python3` dependency is removed. That dependency pulls in `python3[extensions]` and `libb2` on
  non-Windows platforms, and `libb2` needs `autoconf-archive` and `automake`, which the Linux CI images do not provide. The
  portfile still finds a system `python3`. When bumping the vcpkg commit, refresh the copy from upstream and reapply the
  change. `vcpkg.json` lists `./vcpkg_ports` ahead of `./extension-ci-tools/vcpkg_ports` under `overlay-ports`.
- **Platforms.** It builds on the default CI platforms, including Windows with MSVC. These are excluded
  (`exclude_archs` in `.github/workflows/MainDistributionPipeline.yml`): `wasm_mvp`, `wasm_eh` and `wasm_threads` (ecCodes
  does not configure on the community `wasm32-emscripten` triplet) and `windows_amd64_mingw` (ecCodes fails to configure on
  `x64-mingw-static`).
- **`httpfs` in a local binary.** A binary built from the `duckdb` submodule has no published `httpfs`, so `INSTALL httpfs`
  returns a 404. Build it in with a local, git-ignored config. Do **not** add it to `extension_config.cmake`: CI builds every
  extension listed there, and `httpfs` needs libcurl, which is not in this repo's vcpkg manifest. Create
  `extension_config_local.cmake`:

  ```cmake
  duckdb_extension_load(httpfs
      GIT_URL https://github.com/duckdb/duckdb-httpfs
      GIT_TAG a2485c0fd240cbddc0d467019f30201c9a46cf06
  )
  ```

  then `make EXTRA_EXTENSION_CONFIGS=$PWD/extension_config_local.cmake` (this worked on macOS arm64 with the system libcurl),
  and load both by path, `httpfs` first:

  ```sh
  ./build/release/duckdb -unsigned \
    -c "LOAD './build/release/extension/httpfs/httpfs.duckdb_extension';" \
    -c "LOAD './build/release/extension/grib2/grib2.duckdb_extension';"
  ```

## Tests

```sh
make test
```

`make test` runs DuckDB's `run_tests.py`, which needs Python 3.10 or newer. On older Pythons run the binary directly:
`./build/release/test/unittest "test/*"`. Run from the repository root: tests read their data files relative to it.

The grid geometry and the `.idx` and section parsers also have standalone checks that need no DuckDB or ecCodes, run in CI:

```sh
g++ -std=c++17 -O2 -I src/include test/cpp/grid_check.cpp -o /tmp/grid_check && /tmp/grid_check
g++ -std=c++17 -O2 -I src/include test/cpp/idx_check.cpp -o /tmp/idx_check && /tmp/idx_check
```

Test data is described in [fixtures/README.md](fixtures/README.md) and `test/README.md`. Before pushing, run
`make format-check` (or `make format-fix`). CI runs the format check only: the `tidy` check configures the project without
vcpkg, so it cannot find ecCodes.

## More documentation

[docs/DESIGN.md](docs/DESIGN.md) has the design notes: the findings behind the decisions (value ordering, projections, the
`.idx` format, packing survey), measurements, and what is planned next.

## Licenses

This extension is under the MIT License (see `LICENSE`). It links ecCodes (Apache License 2.0) and OpenJPEG (BSD 2-Clause);
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The GRIB2 files in `fixtures/` are unmodified messages from the NOAA NBM
open data; see the [NBM data page](https://registry.opendata.aws/noaa-nbm/) for its terms.

Built from the [DuckDB extension template](https://github.com/duckdb/extension-template).
