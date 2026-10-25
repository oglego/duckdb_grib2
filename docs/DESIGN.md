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

## Next steps

1. `grib2_index()` with grid fingerprint, scanning flags, packing type and `.idx` qualifier columns.
2. `read_grib2()` with `points` and `bbox`, then `.idx` and S3 range reads, then pushdown and parallel scans.

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
