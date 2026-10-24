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
| Closed-form spherical Lambert math matches ecCodes lat/lon for all 3.7M cells to about 1e-13 degrees. | `src/include/grib2_grid.hpp` needs no coordinate arrays. |
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

`src/include/grib2_grid.hpp` is dependency free (no DuckDB, no ecCodes) and provides:

- `LambertGrid::Forward` / `Inverse`: lat/lon to fractional grid coordinates and back (spherical earth, as in NBM).
- `LambertGrid::NearestCell`: rounded cell plus its eight neighbours, ranked by great-circle distance.
- `LambertGrid::CellsInBox`: projects the box outline to get a grid window, then tests each cell exactly.
- `RawValueIndex`: canonical cell to index into ecCodes' `values`.

Not yet covered: the other projections used by NBM regions (Alaska, Hawaii, Puerto Rico, Guam). Check `gridType` for
each with `grib2_index()` before assuming. ecCodes coordinate arrays, cached once per grid, are the fallback.

## Validating the grid code

The reference data in `test/data/grid_reference.txt` is produced entirely by ecCodes (coordinate arrays, iterator and
brute-force searches), never by the code under test. From the repository root:

```sh
pip install eccodes ecmwflibs numpy
python3 test/data/generate_grid_reference.py          # only needed to regenerate the reference
g++ -std=c++17 -O2 -I src/include test/cpp/grid_check.cpp -o /tmp/grid_check && /tmp/grid_check
```

It checks 150 cells (inverse and forward projection, raw value index), 81 nearest-point queries (including points
outside the grid) and 23 boxes (cell counts, including whole-world, antimeridian and outside-grid boxes).
Measured result: maximum coordinate error 8.5e-14 degrees, maximum index error 3.2e-12 cells, 0 failures.

## Next steps

1. `grib2_index()` with grid fingerprint, scanning flags, packing type and `.idx` qualifier columns.
2. Wire the grid code into the extension: load grid parameters from ecCodes keys, check scanning flags, apply
   `RawValueIndex`, and add a SQL test comparing extracted values with the oracle.
3. `read_grib2()` with `points` and `bbox`, then `.idx` and S3 range reads, then pushdown and parallel scans.
