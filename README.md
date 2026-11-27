# DuckDB GRIB2 Extension

**Table of contents**

- [DuckDB GRIB2 Extension](#duckdb-grib2-extension)
- [What is this?](#what-is-this)
- [How do I get it?](#how-do-i-get-it)
  - [Building from source](#building-from-source)
- [Example usage](#example-usage)
- [Supported data](#supported-data)
- [Functions and documentation](#functions-and-documentation)
- [License](#license)

## What is this?

`grib2` is a DuckDB extension for querying GRIB2 weather data with SQL. It uses
[ecCodes](https://github.com/ecmwf/eccodes) to decode values and can read local files or remote files through DuckDB's
`httpfs` extension.

> **Scope and limitations (v0.1).** Verified on NOAA NBM (CONUS, Alaska, Hawaii, Puerto Rico, Guam) and the HRRR
> CONUS surface file: Lambert conformal, north polar stereographic and Mercator grids, with packing templates 5.0, 5.2
> and 5.3. **Regular latitude/longitude grids, which GFS and many other GRIB2 files use, are not supported yet**, nor are
> Gaussian grids (ECMWF), JPEG 2000 or CCSDS packing. Unsupported messages fail with an error; `grib2_index` can still
> list their metadata. The [support matrix](docs/SUPPORT.md) shows exactly what works, what is refused on purpose, and
> what is not done yet.
>
> **Use trusted files only.** The extension checks each message's structure before ecCodes decodes it, and the parsers
> are fuzzed in CI, but ecCodes itself trusts its input and a decoder crash on a deliberately malformed file cannot be
> ruled out. Do not point it at files uploaded by people you do not trust without sandboxing the process. See
> [untrusted input](docs/OPERATIONS.md#untrusted-input).

### Examples

```bash
memory D SELECT count(*) FROM read_grib2('https://noaa-hrrr-bdp-pds.s3.amazonaws.com/hrrr.20261107/conus/hrrr.t00z.wrfsfcf00.grib2', all_cells := true);
┌──────────────────┐
│   count_star()   │
│      int64       │
├──────────────────┤
│    323873970     │
│ (323.87 million) │
└──────────────────┘
Run Time (s): real 26.630 user 27.202246 sys 0.109362
```

```bash
memory D SELECT count(*) FROM read_grib2('https://noaa-hrrr-bdp-pds.s3.amazonaws.com/hrrr.20261107/conus/hrrr.t00z.wrfsfcf00.grib2', bbox := [-93.4116, 37.1352, -93.1843, 37.2863], search := ':TMP:2 m|UGRD:10 m|VGRD:10 m|APCP|PRMSL');
┌──────────────┐
│ count_star() │
│    int64     │
├──────────────┤
│          148 │
└──────────────┘
Run Time (s): real 0.832 user 0.614036 sys 0.010879
```

## How do I get it?

### Building from source

Build the extension locally and load the resulting unsigned binary with DuckDB's `-unsigned` flag. See
[Building from source](docs/BUILDING.md) for prerequisites, build and test commands, and instructions for loading it.

## Example usage

Start DuckDB with `-unsigned` and load your local build by path, as shown in the
[build guide](docs/BUILDING.md). Then inspect a file's messages and read a time series at a point:

```sql
LOAD './build/release/extension/grib2/grib2.duckdb_extension';

SELECT message_number, variable, level, forecast
FROM grib2_index('path/to/file.grib2')
LIMIT 10;

SELECT valid_time, round(value, 1) AS kelvin
FROM read_grib2(
    'blend.t00z.core.f00[1-6].co.grib2',
    search := '^TMP:2 m above ground:\d+ hour fcst$',
    points := [{'lat': 37.21, 'lon': -93.29}]
)
ORDER BY valid_time;
```

Both functions also support `https://` and `s3://` paths when DuckDB's `httpfs` extension is loaded. See
[Building from source](docs/BUILDING.md) for local `httpfs` setup and [operational notes](docs/OPERATIONS.md) for
remote-read details.

## Supported data

Verified inputs are NBM forecast-hour-1 core products for CONUS, Alaska, Hawaii, Puerto Rico, and Guam, plus the HRRR
CONUS surface file. Grids, packing templates and features are listed in the [support matrix](docs/SUPPORT.md), which
marks each one as supported, refused on purpose, or not done yet. Anything outside it is refused with an error.

## Functions and documentation

- `grib2_index(path, idx := true, details := false)` lists message inventory and metadata.
- `read_grib2(path, message := ..., search := ..., points := ..., bbox := ..., wrap := false, all_cells := false)` decodes selected
  values at points, within a bounding box, or across a grid.
- BLOB helpers: `grib2_eccodes_version`, `grib2_message_info`, and `grib2_point_value`.

- [Function and column reference](docs/REFERENCE.md): every output column of `read_grib2` and `grib2_index`, where each
  value comes from, when it is NULL, and how `valid_time` is derived.
- [Building from source](docs/BUILDING.md): dependencies, local builds, and tests.
- [Support matrix](docs/SUPPORT.md): which grids, packing templates, data sources and features are supported, refused,
  or still to do.
- [Operations](docs/OPERATIONS.md): runtime settings, remote reads, `.idx` rules, and input caveats.
- [Test data and coverage](test/README.md).

## License

This extension is under the MIT License (see [LICENSE](LICENSE)). It links ecCodes (Apache License 2.0) and OpenJPEG
(BSD 2-Clause); see [third-party notices](THIRD_PARTY_NOTICES.md). GRIB2 fixtures are from NOAA's open NBM and HRRR
data; see the [NBM](https://registry.opendata.aws/noaa-nbm/) and [HRRR](https://registry.opendata.aws/noaa-hrrr-pds/)
data pages for terms.

Built from the [DuckDB extension template](https://github.com/duckdb/extension-template).
