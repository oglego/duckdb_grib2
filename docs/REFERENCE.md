# Function and column reference

This page lists what each function returns. For worked examples see the [README](../README.md); for supported data and
limits see [OPERATIONS](OPERATIONS.md). The same function signatures are available in SQL with
`SELECT * FROM duckdb_functions() WHERE function_name IN ('read_grib2', 'grib2_index');`.

Two things decide most of what is filled in:

- **Whether an `.idx` inventory sits next to the file** (`file.grib2.idx`). The variable, level, forecast and qualifier
  names, and everything derived from them, come only from the inventory. Without one they are NULL.
- **Whether the metadata comes from the inventory or from the message itself.** The inventory is cheap to read; the
  message headers need a small range read per message.

## `read_grib2`

```sql
read_grib2(path, message := NULL, search := NULL, points := NULL, bbox := NULL, all_cells := false)
```

Decodes the selected messages into one row per selected grid cell. Choose cells with exactly one of `points` (the
nearest cell to each point), `bbox` (every cell whose centre is inside `[lon_min, lat_min, lon_max, lat_max]`) or
`all_cells := true`. Choose messages with `message` (message numbers) and/or `search` (a regular expression matched
against the `.idx` inventory text).

| Column | Type | Meaning |
|---|---|---|
| `file_path` | VARCHAR | The file the row came from. |
| `message_number` | BIGINT | Position of the message in the file, counting from 1. |
| `reference_time` | TIMESTAMP | The model run time. From the `.idx` (to the minute) when one is used, otherwise from the message. |
| `valid_time` | TIMESTAMP | `reference_time` plus `step_hours`, see [Forecast steps](#forecast-steps). NULL without an `.idx`, or when the forecast text gives no step. |
| `variable` | VARCHAR | From the `.idx` line, for example `TMP`. NULL without an `.idx`. |
| `level` | VARCHAR | From the `.idx` line, for example `2 m above ground`. NULL without an `.idx`. |
| `forecast` | VARCHAR | From the `.idx` line, for example `1 hour fcst` or `0-6 hour acc fcst`. NULL without an `.idx`. |
| `qualifier` | VARCHAR | Anything after the forecast on the `.idx` line, for example `50% level` or `prob >0.254`. Empty (not NULL) when the line has none; NULL without an `.idx`. |
| `short_name` | VARCHAR | ecCodes' short name for the field, for example `2t`. NULL if ecCodes has none. |
| `lat`, `lon` | DOUBLE | The centre of the cell in degrees. `lon` is in -180 to 180. |
| `i`, `j` | BIGINT | The cell's column and row on the grid, counting from 0. `i` increases east and `j` increases north, and cell (0, 0) is the grid's first point. This does not depend on how the file stores its rows. |
| `value` | DOUBLE | The value in the message's own units (for example kelvin for temperature; nothing is converted). NULL where the message has no data for the cell. |
| `query_lat`, `query_lon` | DOUBLE | `points` only: the point that was asked for. NULL for `bbox` and `all_cells`. |
| `distance_km` | DOUBLE | `points` only: the great-circle distance from the requested point to the cell's centre, in kilometres. NULL for `bbox` and `all_cells`. |

A point outside a message's grid produces no row for that message. Cells with no data (masked by a bitmap, or flagged
missing by complex packing) have `value` NULL, and a real value of 9999 is kept as 9999.

## `grib2_index`

```sql
grib2_index(path, idx := true, details := false)
```

One row per message, without decoding any values. `path` may be a glob. `idx := false` ignores any `.idx` file, and
`details := true` also reads the head of every message to fill the columns marked below.

| Column | Type | Filled from | Meaning |
|---|---|---|---|
| `file_path` | VARCHAR | always | The file the message is in. |
| `message_number` | BIGINT | always | Position in the file, counting from 1. |
| `byte_offset`, `byte_length` | BIGINT | always | Where the message starts and how long it is. Enough for a range request. |
| `reference_time` | TIMESTAMP | always | From the `.idx` when there is one, otherwise from the message. |
| `variable`, `level`, `forecast`, `qualifier` | VARCHAR | `.idx` | The four parts of the inventory line. NULL without an `.idx`; `qualifier` is empty when the line has none. |
| `inventory` | VARCHAR | `.idx` | `variable:level:forecast`, plus `:qualifier` when there is one. This is the text `search` matches. NULL without an `.idx`. Not unique within a file, so `message_number` is the key. |
| `step_hours` | BIGINT | `.idx` | Hours from `reference_time` to the time the field is valid, see [Forecast steps](#forecast-steps). |
| `percentile` | DOUBLE | `.idx` | `N` for a qualifier of the form `N% level`, otherwise NULL. |
| `threshold_lower`, `threshold_upper` | DOUBLE | `.idx` | Bounds of a probability qualifier: `prob >N` sets the lower, `prob <N` the upper, and `prob >=A <B` both. NULL otherwise. |
| `discipline`, `parameter_category`, `parameter_number` | INTEGER | message headers | The GRIB2 parameter identifiers. |
| `grid_template`, `packing_template` | INTEGER | message headers | GRIB2 grid definition and data representation template numbers. |
| `packing_type` | VARCHAR | message headers | A name for the packing template, or `template N` when the extension has none. |
| `short_name`, `type_of_level`, `level_value` | VARCHAR, VARCHAR, DOUBLE | ecCodes (`details := true`) | The field's short name, level type and level value. |
| `grid_type`, `ni`, `nj`, `alternative_row_scanning` | VARCHAR, BIGINT, BIGINT, BOOLEAN | ecCodes (`details := true`) | The grid type (for example `lambert`), its width and height in cells, and whether alternate rows are stored in opposite directions. |

When the `.idx` is used, the "message headers" columns are NULL unless `details := true`: the headers are not read at
all. Without an `.idx` they are always filled, because the file is walked message by message to find each one. The
ecCodes columns need `details := true` in both cases. A message whose headers claim more data points than the extension
will ever decode is still listed, but its ecCodes columns stay NULL.

## Forecast steps

`step_hours` comes from the `forecast` text of the `.idx` line, and `valid_time` in `read_grib2` is
`reference_time + step_hours`.

| `forecast` text | `step_hours` | `valid_time` |
|---|---:|---|
| `anl` | 0 | the reference time |
| `N hour fcst` | N | reference time + N hours |
| `a-b hour ...` with `a <= b`, such as `0-6 hour acc fcst`, `6-12 hour max fcst` or `0-1 hour acc@(fcst,dt=1 hour),missing=0` | b | reference time + b hours: the end of the interval |
| `15 min fcst`, `0-1 day acc fcst`, `b < a`, or anything else | NULL | NULL |
| a step beyond 1,000,000 hours | NULL | NULL |

An accumulation, maximum or average describes the hours from `a` to `b`, and it is valid when that period ends. This is
how cfgrib and Herbie treat such fields too. `a` itself is not kept in a column; it is in the `forecast` text.

Without an `.idx` the extension does not know the step, so `valid_time` is NULL. Put an inventory next to the file (or
compute the time from `reference_time` yourself) when you need it.

## Helper functions

| Function | Returns |
|---|---|
| `grib2_eccodes_version()` | The version of the linked ecCodes, for example `2.47.0`. |
| `grib2_message_info(content BLOB)` | A one-line description of the first GRIB2 message in a buffer: edition, short name, level, grid, packing and the minimum and maximum value. |
| `grib2_point_value(content BLOB, lat DOUBLE, lon DOUBLE)` | The value at the cell nearest to a point, or NULL if the point is outside the grid. Decodes the whole message for every row, so use `read_grib2` for more than a spot check. |

Both BLOB helpers take bytes from `read_blob()`, for example
`SELECT grib2_point_value(content, 37.21, -93.29) FROM read_blob('file.grib2')`.
