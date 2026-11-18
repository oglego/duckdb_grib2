# Supported data and limits

This page describes the data combinations verified by the project and the main runtime limits. Other GRIB2 files may
work, but have not been qualified. For SQL examples, see the [README](../README.md).

## Verified data and formats

The current verified inputs are NBM forecast-hour-1 core products for CONUS, Alaska, Hawaii, Puerto Rico, and Guam, and
the HRRR CONUS surface file `hrrr.t{HH}z.wrfsfcf00.grib2`.

`read_grib2`, `grib2_message_info`, and `grib2_point_value` currently support:

- Lambert conformal, north polar stereographic, and Mercator grids on a spherical earth.
- The usual scan order: rows run west to east and south to north, optionally alternating row direction.
- Packing templates 5.0 (simple), 5.2 (complex), and 5.3 (complex with spatial differencing).
- One field per GRIB2 message.

Other projections and scan orders are rejected. Regular latitude/longitude grids, ellipsoidal earths, south polar
stereographic grids, JPEG 2000 and CCSDS packing, and multi-field messages are not supported. `grib2_index` can still
list messages using unsupported grids or packing templates, but decoding them is refused.

The extension accepts NOAA-style wgrib2 `.idx` inventories. Entries such as `n.m`, which represent several fields in one
message, are not supported. If no sidecar is available, the file is scanned message by message; `search := ...` requires
an available `.idx` inventory.

## Runtime settings

```sql
SET grib2_io_threads = 8;
```

The default is 1; the allowed range is 1–64. Values above 1 overlap file reads for `grib2_index` and prefetch message
bytes for `read_grib2` (up to six messages). Decoding remains single-threaded and row order stays deterministic. Parallel
file access has been exercised with HTTPS and public S3, but not every filesystem extension or DuckDB secret setup.
Queries that stop early may wait for reads already in flight.

## Resource limits

Inputs that exceed these limits fail with an error:

| Resource | Limit |
|---|---:|
| `.idx` inventory | 64 MiB |
| GRIB2 message | 256 MiB |
| Grid cells or claimed data points | 64 million |
| Header sections read while indexing or decoding | 1 MiB |
| Search expression | 1024 characters |
| Prefetched message bytes | 128 MiB (at least one message may be in flight) |
| Indicator-section message length | 2^62 bytes |
| Forecast step parsed from `.idx` | 0–1,000,000 hours |

A forecast step outside the supported range is left unset, so `valid_time` is NULL. Grid dimensions, spacing, coordinates,
and earth radius are checked before projection math is used.

## Missing values and input safety

Cells marked missing by a bitmap or by complex packing are returned with a SQL `NULL`. The decoder configures ecCodes to
use NaN as its missing marker, which preserves a legitimate value of 9999.

The `.idx` and section-header parsers are fuzzed and sanitizer-checked in CI. Before a complete message reaches ecCodes,
the extension validates its sections, counts, and supported packing layout. However, ecCodes still parses the full
message, and its decoder is mutation-tested against the CI distribution's ecCodes rather than the exact version built
into this extension. Malformed inputs should generally produce an error, but a decoder crash cannot be ruled out; use
trusted data when possible.

For a URL with a query string, the extension appends `.idx` to the full URL, so a sidecar is generally not found and
`search` is unavailable. Use `message := ...` to select by message number instead. Files without an `.idx` are scanned
from the beginning, and truncated messages produce an error.

## Performance expectations

Every selected message is decoded in full, even when the query requests one point or a small box. Spatial filters reduce
the number of output rows, not the decode work. Remote reads can overlap when `grib2_io_threads` is greater than 1; value
decoding remains single-threaded.
