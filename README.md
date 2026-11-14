# grib2: GRIB2 weather data in DuckDB

A DuckDB extension for querying GRIB2 files with SQL. List the messages in a file, then read decoded values at the points
or inside the bounding box that you are interested in, from local files or from object storage such as the
[NOAA National Blend of Models (NBM)](https://registry.opendata.aws/noaa-nbm/) bucket. Decoding is done by
[ecCodes](https://github.com/ecmwf/eccodes).

> **Status: early.** The extension works on local files and, through `httpfs`, on public NBM data. The current verified
> scope is the NBM fixtures and public-bucket checks documented in this repo. The HRRR S3 path shown in the examples
> (`s3://noaa-hrrr-bdp-pds/hrrr.{YYYYMMDD}/conus/hrrr.t{HH}z.wrfsfcf00.grib2`) is a working example target, not yet part of
> the formally supported meteorological data matrix. The extension has not been published to the DuckDB
> community-extensions repository and is not ready for general production use. See the
> [release-readiness checklist](docs/RELEASE_READINESS.md) for what remains before release.

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
is a regular expression (DuckDB's RE2 syntax) matched against the sidecar's inventory text, so it needs the `.idx`. The text
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

The current verified scope is NBM forecast-hour-1 core-product data for CONUS, Alaska, Hawaii, Puerto Rico and Guam.
This is the dataset covered by the real fixture set and SQL test cases in this repository. It supports Lambert conformal,
north polar stereographic and Mercator grids on a spherical earth, with the usual NBM scanning modes. Other grid types
and scanning modes are not supported.

The HRRR S3 path `s3://noaa-hrrr-bdp-pds/hrrr.{YYYYMMDD}/conus/hrrr.t{HH}z.wrfsfcf00.grib2` is used in the examples as
an active future target and has been exercised manually, but it is not yet part of the documented supported-product matrix
for release claims. Other sources and product types have not yet been verified.

See [Supported inputs and operational limits](docs/OPERATIONS.md) for details about packing, inventory files, limits,
and data that has not been tested.

## Settings, limits and untrusted input

```sql
SET grib2_io_threads = 8;   -- default 8, range 1 to 64
```

`grib2_index` and `read_grib2` overlap remote reads using background threads. Set this to `1` to keep reads on the query
thread if a filesystem extension or environment does not support background reads:

```sql
SET grib2_io_threads = 1;
```

See [operational notes](docs/OPERATIONS.md) for resource limits, input caveats, and guidance on untrusted files.

## Performance

Every selected message is decoded in full, so spatial filters reduce output rows and memory, not decode time. Remote
reads are overlapped; see the [measured examples](docs/OPERATIONS.md#performance).

## Installing

There is no published release yet. Until the extension is in the community-extensions repository, build it from source
and load the binary by path (see below). Once published:

```sql
INSTALL grib2 FROM community;
LOAD grib2;
```

## Building and testing

Build from source with `make`; run the SQL tests with `make test`. See [Building from source](docs/BUILDING.md) for
dependencies, loading a local build, platform notes, and test alternatives. Test fixtures and coverage are described in
[test/README.md](test/README.md).

## More documentation

- [Supported inputs and operational limits](docs/OPERATIONS.md): compatibility, limits, and performance notes.
- [Building from source](docs/BUILDING.md): dependencies, local builds, and test commands.
- [ecCodes integration](docs/ECCODES.md): the decoder boundary, keys, and API calls.
- [Design notes](docs/DESIGN.md): findings behind value ordering, projections, the `.idx` format, and packing support.

## Licenses

This extension is under the MIT License (see `LICENSE`). It links ecCodes (Apache License 2.0) and OpenJPEG (BSD 2-Clause);
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The GRIB2 files in `fixtures/` are unmodified messages from the NOAA NBM
open data; see the [NBM data page](https://registry.opendata.aws/noaa-nbm/) for its terms.

Built from the [DuckDB extension template](https://github.com/duckdb/extension-template).
