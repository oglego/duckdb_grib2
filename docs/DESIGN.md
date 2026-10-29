# Design notes

Working notes for `grib2_index()` and `read_grib2()`. Numbers marked "measured" come from the NBM CONUS 2 m temperature
message in `fixtures/` (3,744,965 points), decoded with ecCodes 2.50 through its Python bindings. Timings are rough and
machine dependent. Re-check anything marked "to verify" against the ecCodes 2.47 build used by the extension.

## Priorities

Most users want a few places, not a whole domain. Spatial selection comes first:

1. **Points**: nearest grid cell for one or more lat/lon pairs.
2. **Bounding box**: every cell whose centre lies inside a lat/lon box.
3. Later: `WHERE lat/lon` predicate pushdown, radius selection, bilinear interpolation, a table-in variant for thousands
   of stations.

Sketch (not implemented):

```sql
-- nearest cell to a point, for every matching message
SELECT valid_time, query_lat, query_lon, lat, lon, distance_km, value
FROM read_grib2('s3://noaa-nbm-grib2-pds/.../*.grib2',
                search := 'TMP:2 m above ground',
                points := [{'lat': 37.21, 'lon': -93.29}]);

-- every cell inside a box (lon_min, lat_min, lon_max, lat_max, the same order as DuckDB spatial)
SELECT valid_time, lat, lon, value
FROM read_grib2('s3://.../blend.t00z.core.f001.co.grib2',
                search := 'TMP:2 m above ground',
                bbox := [-95.0, 36.0, -92.0, 38.0]);
```

Selecting messages follows Herbie's workflow: an inventory (`grib2_index()`, one row per message) and a search over it.
Output is long format (one row per selected cell per message) because DuckDB users expect tables, joins and `COPY` to
Parquet. Return query coordinates alongside the matched cell so results join back to a stations table.

## Findings that shape the implementation

| Finding | Consequence |
|---|---|
| Decoding one message takes about 320 ms (measured). Complex packing with spatial differencing is sequential. | A spatial filter cuts output rows and memory, not decode time. Parallelism across messages is the real speedup. |
| ecCodes `latitudes`/`longitudes` arrays take about 560 ms and 420 ms per message (measured), more than decoding. | Never compute coordinate arrays per message. Use closed-form projection math, or compute once per grid and cache. |
| ecCodes' nearest-point search took about 2.3 s on first call (measured). | Do not use it per message. |
| ecCodes returns longitudes in 0..360 (221 to 301 for CONUS). | Accept both conventions on input, return -180..180, handle boxes crossing the antimeridian. |
| `alternativeRowScanning = 1` in this message. **`values` is in stored order (odd rows east to west) while latitudes and longitudes are canonical row-major.** | Pairing `values[k]` with `lat[k]` silently mirrors every other row. See below. |
| Closed-form spherical Lambert, polar stereographic and Mercator math matches ecCodes lat/lon for every cell of every NBM region (CONUS, Alaska, Hawaii, Puerto Rico, Guam) to better than 1e-11 degrees. | `src/include/grib2_grid.hpp` needs no coordinate arrays. |
| All messages in an NBM file share one grid. | Build grid structures once per distinct grid, keyed by a fingerprint stored in `grib2_index()`. |
| In the `.idx`, many messages share a variable and level and differ by percentile, probability threshold or `ens std dev`. | `grib2_index()` needs a qualifier column and the raw `.idx` text, not just variable and level. |

### The value-order trap

This is the most dangerous finding because nothing fails: the output is plausible and wrong for half the rows.

Evidence (measured, ecCodes 2.50):

- With `values` reshaped as `(Nj, Ni)`, the mean absolute difference between vertically adjacent cells is 4.33 K versus
  0.24 K horizontally. After reversing odd rows it is 0.22 K.
- ecCodes' geography iterator, which yields aligned `(lat, lon, value)`, matches the odd-row-reversed `values` and the
  canonical coordinate arrays for every cell checked.
- ecCodes' nearest-point value matched the corrected array in all 8 spot checks, and differed from the raw array for
  those on odd rows.

`RawValueIndex()` in `grib2_grid.hpp` encodes the mapping. `test/data/generate_grid_reference.py` asserts it against the
iterator on the first four rows. **To verify:** whether the same holds in ecCodes 2.47 as built by the extension. The
decode path must be tested against the iterator or a wgrib2 dump before `read_grib2()` is trusted. Only the scanning
mode of this grid (`iScansNegatively = 0`, `jScansPositively = 1`, `jPointsAreConsecutive = 0`) is handled; the loader
must reject anything else.

## Grid math

`src/include/grib2_grid.hpp` is dependency free (no DuckDB, no ecCodes). NBM publishes separate files per region and they
do not share a projection (surveyed with `grib2_message_info`; all use the same packing and the same stored-row order):

| Region | Projection | Size |
|---|---|---|
| CONUS | Lambert conformal | 2345 x 1597 |
| Alaska | north polar stereographic | 1649 x 1105 |
| Hawaii, Puerto Rico, Guam | Mercator | 625 x 561, 339 x 225, 193 x 193 |

All of them use a spherical earth (`shapeOfTheEarth = 1`, radius 6,371,200 m) and scanning mode 80
(`iScansNegatively = 0`, `jScansPositively = 1`, alternating row direction).

`ProjectedGrid` implements the logic once, in terms of a projection's `Project` and `Unproject`:

- `Forward` / `Inverse`: lat/lon to fractional grid coordinates and back.
- `NearestCell`: rounded cell plus its eight neighbours, ranked by great-circle distance.
- `CellsInBox`: projects the box outline to get a grid window, then tests each cell exactly.
- `RawValueIndex`: canonical cell to index into ecCodes' `values`.

`LambertGrid`, `PolarStereographicGrid` and `MercatorGrid` supply only the projection math. The extension builds the
right one from the message's grid keys and rejects anything else (other grid types, ellipsoidal earths, south polar,
other scanning modes) with a clear error.

Two details found by checking against ecCodes:

- **Mercator orientation is ignored.** ecCodes reports `orientationOfTheGridInDegrees` as 0, 200 and 295 for Guam,
  Hawaii and Puerto Rico, but a plain Mercator with spacing true at `LaD` and the first grid point as origin reproduces
  ecCodes' coordinates to about 1e-13 degrees without it.
- **Polar stereographic** uses `orientationOfTheGridInDegrees` as the longitude pointing down the map, with `LaD` as
  the true latitude (60 degrees for Alaska).

## Validating the grid code

The reference data in `test/data/grid_reference.txt` is produced entirely by ecCodes (coordinate arrays, iterator and
brute-force searches), never by the code under test. It has one section per region, built from the fixtures in
`fixtures/`. From the repository root:

```sh
pip install eccodes ecmwflibs numpy
python3 test/data/generate_grid_reference.py          # only needed to regenerate the reference
g++ -std=c++17 -O2 -I src/include test/cpp/grid_check.cpp -o /tmp/grid_check && /tmp/grid_check
```

Per region it checks 100 cells (inverse and forward projection, raw value index), 33 nearest-point queries (25 inside the
grid, 8 outside) and 15 boxes (cell counts, including whole-world, antimeridian-style, far-away and whole-region boxes).
Measured result across the five regions: maximum coordinate error 2e-12 degrees, maximum index error 2.3e-11 cells,
0 failures. The generator also asserts the stored-row-order claim against ecCodes' geography iterator for each region.

Box edges in the reference are offset from cell centres by a fraction of a cell. An edge exactly on a centre, or on a
whole row of centres (every cell in a Mercator row shares one latitude), would make membership depend on 1e-14
rounding noise instead of on the code under test.

## `grib2_index()` design

Findings from the real NBM `.idx` (154 messages, `fixtures/blend.t00z.core.f001.co.grib2.idx`) that shape the index:

- **Lines are irregular.** They have 7, 8 or 9 colon-separated fields, qualifiers can contain colons
  (`prob <304.8:prob fcst 3/7`), and some lines have no trailing colon. Only the first six fields are split on colons;
  the rest of the line is the qualifier.
- **The idx text is not a unique key.** There are 148 distinct (variable, level, forecast, qualifier) combinations
  among 154 messages; `CDCB`, `TCDC` and `CDCTOP` at `reserved` / `1 hour fcst` each appear three times. Message number
  (and byte offset) is the only reliable identifier, so a `search` can match several messages unless ecCodes details
  disambiguate them.
- **Qualifier forms:** none (52), percentile `N% level` (58), probability (37: 23 `prob >N`, 9 `prob <N`, 4
  `prob >=A <B`, and one bare `probability forecast`), `ens std dev` (6), and one other. The raw text is kept; the
  parser also derives `percentile` and the probability bounds.
- **Forecast text is not always `N hour fcst`.** It also has accumulations (`0-1 hour acc fcst`), maxima and a
  `missing` form, so `step_hours` is only set for `anl` (0) and `N hour fcst`.
- **The last message's length needs the file size**, because it has no following offset. Message sizes in this file range
  from 179 KB to 3.8 MB.
- Lines such as `1.2:...` (several fields in one message) are rejected with a clear error for now.

`src/include/grib2_idx.hpp` implements the parser and `src/include/grib2_message.hpp` reads section headers, giving the
packing template, parameter and grid template from a few hundred bytes of a message (ecCodes' partial handle cannot
provide the packing template). Both are tested in `test/cpp/idx_check.cpp`: the real `.idx` against an independent
regex implementation (154 messages, every column), section headers of all fixtures against ecCodes (including parsing
from growing prefixes, as a ranged reader would), and `test/data/multi.grib2` where sidecar offsets must agree with
the message boundaries found by hopping through the indicator sections.

Implemented as `grib2_index(path, idx := true, details := false)` in `src/grib2_index_function.cpp`. `path` may be a glob.
Rows come from the first of these that applies:

1. **The `.idx` sidecar** (`<path>.idx`, when it exists and `idx` is true): one small read. The first message is checked
   against the sidecar (every message with `details`), so an `.idx` that does not belong to the file gives an error
   instead of wrong offsets.
2. **Header hopping** (no sidecar, or `idx := false`): reads a short prefix of each message, using the length in the
   indicator section to find the next one. No names, but the parameter numbers, grid and packing templates and
   reference time come from the section headers.
3. **`details := true`** additionally gives ecCodes' names, level and grid details, from the same short prefix (192 bytes
   for every fixture here) through its partial-message API.

| Column | Source |
|---|---|
| `file_path`, `message_number`, `byte_offset`, `byte_length` | `.idx`, or header hopping |
| `reference_time` | `.idx` `d=` field, or section 1 |
| `variable`, `level`, `forecast`, `qualifier`, `inventory` | `.idx` text (`inventory` is `variable:level:forecast[:qualifier]`, for searching) |
| `step_hours`, `percentile`, `threshold_lower`, `threshold_upper` | parsed from the `.idx` text |
| `discipline`, `parameter_category`, `parameter_number`, `grid_template`, `packing_template`, `packing_type` | section headers (hopping or `details`) |
| `short_name`, `type_of_level`, `level_value`, `grid_type`, `ni`, `nj`, `alternative_row_scanning` | ecCodes (`details := true`) |

Columns that do not apply to a mode are NULL. `packing_type` uses ecCodes' names; each is checked against ecCodes in
`test/cpp/idx_check.cpp` (nine gridded templates, including JPEG 2000 and CCSDS), and unknown templates show as
`template N`. All reads go through DuckDB's file system, so `s3://` and `https://` should work once `httpfs` is loaded.
That has not been tried yet; the tests use local files only.

SQL tests in `test/sql/grib2.test` cover the sidecar, hopping, details, globs, and the error cases (missing file, not a
GRIB file, and a stale `.idx`, using `test/data/stale.grib2`).

**Survey result (NBM f001 core, all five regions):** only templates 5.3 (complex with spatial differencing) and 5.2
(complex) occur, and the grid type is constant within a file. Checked with `grib2_index(..., details := true)`. The
sidecar and an independent walk through all 154 CONUS messages agree on every offset and length. See
`fixtures/README.md`.

**Masked cells.** Real NBM messages do not always have bitmaps (none of the fixtures here do), so
`test/data/masked_gu.grib2` is a synthetic Guam message with a bitmap masking 400 cells. ecCodes fills masked cells with
its missing value (9999). `grib2_point_value` returns NULL for them and `grib2_message_info` leaves them out of min and
max; `read_grib2` must do the same.

## `read_grib2()` as built

`read_grib2(path, message := ..., search := ..., points := ..., bbox := ..., all_cells := false)` in
`src/grib2_read_function.cpp`. It reuses `IndexFile` (the same message index as `grib2_index`) to find messages, then reads
and decodes each selected message in turn.

- **Which messages:** all of them by default; `message := 133` or `[133, 134]` selects by number; `search := '...'` is a
  regular expression (ECMAScript syntax, from `std::regex`, not RE2) matched against the `.idx` inventory text and needs the
  sidecar. Both can be combined.
- **Which cells:** exactly one of `points` (nearest cell to each `{'lat', 'lon'}`), `bbox` (`[lon_min, lat_min, lon_max,
  lat_max]`, every cell centre inside) or `all_cells := true`. With none the function errors, because one CONUS message has
  3.7 million cells.
- **Output:** `file_path`, `message_number`, `reference_time`, `valid_time` (reference time plus `step_hours` when the
  forecast is a simple one), `variable`, `level`, `forecast`, `qualifier` (from the `.idx`, NULL without one), `short_name`
  (ecCodes), `lat`, `lon`, `i`, `j`, `value`, and in points mode `query_lat`, `query_lon` and `distance_km`.
- **Masked cells** have a NULL `value`. **Points outside the grid produce no row** (a point inside a message's grid but on
  a masked cell does produce one, with a NULL value).
- **Cost:** about one decode per message (roughly 320 ms for a CONUS message in the sandbox measurements above), whatever
  the spatial selection, because a spatial filter cannot skip decoding. Single-threaded for now. Cell selection (point
  lookups, box windows) is computed once and reused while consecutive messages share a grid (compared by a fingerprint of
  the grid parameters), which is the case for every message in an NBM file.
- **Not yet:** S3 reads (untested), `WHERE lat/lon` pushdown, parallel scans, bilinear interpolation.

Tests cover points on odd and even rows against ecCodes' own nearest-point values, input order, points outside the grid,
`search` and `message` selection with `valid_time`, a bbox and a whole grid over the synthetic masked message
(`test/data/masked_gu.grib2`), and the error cases.

## Next steps

1. Try `grib2_index()` and `read_grib2()` against S3 (NBM and the HRRR example). This needs an `httpfs` build for the dev
   binary; see the README.
2. Parallel scans (one message per work unit) after checking ecCodes' thread safety, then `WHERE lat/lon` pushdown.
3. Bilinear interpolation, a table-in variant for thousands of stations, and other grid types (regular lat/lon for GFS).

## Other sources to try later

Not needed yet. NBM covers the first milestones, and these are checks to run once `grib2_index()` and `read_grib2()`
exist.

**HRRR** (Lambert conformal, 1799 x 1059, about 3 km) should work with the existing grid code. Example endpoint:

- Index: <https://noaa-hrrr-bdp-pds.s3.amazonaws.com/hrrr.20140730/conus/hrrr.t18z.wrfnatf00.grib2.idx>
- Data: the same URL without `.idx`

Caveats about this particular example:

- It is from 2014-07-30 18z, which is early HRRR. Packing and earth shape may differ from current files (an HRRR
  dataset from that era reports a 6,367,470 m earth radius, shape 0; the extension accepts shapes 0, 1, 6 and 8).
- `wrfnat` is the native-level product, so expect many messages. `wrfsfc` and `wrfprs` are the more common choices.
- Its contents are unverified. The URL resolves, but it came back as binary and the `.idx` has not been inspected.
- The bucket layout (`noaa-hrrr-bdp-pds`, `.idx` beside each file) is from this example, not yet confirmed by a listing.

What to check when trying it:

1. `.idx` line format versus NBM's (`n:offset:d=YYYYMMDDHH:VAR:level:fcst:`).
2. `packingType` across messages. One third-party HRRR decoder describes templates 5.3 (complex with spatial
   differencing) and 5.0 (simple); only 5.3 is proven here.
3. `shapeOfTheEarth`, scanning flags and `alternativeRowScanning`.
4. `grib2_point_value` against an ecCodes nearest-point lookup, as for the NBM fixtures.

Grids not supported yet: regular lat/lon (for example GFS) is a trivial projection and cheap to add; Gaussian grids
(for example ECMWF) are real work.
