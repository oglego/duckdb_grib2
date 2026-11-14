# Supported inputs and operational limits

This note expands on the support summary in the [README](../README.md#what-it-reads). Support is intentionally limited to
the combinations tested here; other GRIB2 data may work but has not been qualified.

## Supported inputs

- **Grids:** Lambert conformal (NBM CONUS), north polar stereographic (NBM Alaska), and Mercator (NBM Hawaii, Puerto Rico,
  Guam), on a spherical earth, with the usual scanning mode (rows stored west to east and south to north, with or without
  alternating row direction). Other grid types (for example regular latitude/longitude, as in GFS), ellipsoidal earths,
  south polar grids, and other scanning modes are not supported.
- **Packing:** ecCodes decodes the values. The packing templates exercised by the tests are 5.3 (complex with spatial
  differencing) and 5.2 (complex). Other templates are passed to ecCodes but are not tested here. JPEG 2000 support
  (OpenJPEG) is built in but untested; CCSDS is not built in.
- **Inventory files:** the NOAA-published wgrib2-style `.idx` format. Lines of the form `n.m` (several fields in one
  message) are not supported yet.
- **Current verified data:** NBM CONUS, Alaska, Hawaii, Puerto Rico, and Guam, forecast hour 1 of the core product.
  This is the tested matrix in this repository. The HRRR path
  `s3://noaa-hrrr-bdp-pds/hrrr.{YYYYMMDD}/conus/hrrr.t{HH}z.wrfsfcf00.grib2` is a working example target and a future
  expansion candidate, but it is not part of the formally supported dataset matrix until it has fixture coverage and
  independent-oracle validation.

## Settings and limits

```sql
SET grib2_io_threads = 8;   -- default 8, range 1 to 64
```

`grib2_index` and `read_grib2` overlap network round trips using background threads. They call DuckDB's file system from
those threads rather than from DuckDB's own. If a filesystem extension misbehaves under that, or the environment
restricts thread creation, set `grib2_io_threads = 1` to keep reads on the query thread. This is slower for remote files
but returns the same results.

Hard resource and input limits are reported as errors naming the file and message where applicable. A forecast step
outside the supported range is left unset, so `valid_time` is NULL:

| Limit | Value |
|---|---|
| `.idx` file size | 64 MiB |
| One message | 1 GiB |
| Grid size (`Ni x Nj`) | 250 million cells |
| `search` pattern length | 1024 characters |
| Message bytes held in flight by prefetching | 128 MiB (at least one message) |
| Grid or data points a message may claim (checked before ecCodes sees it) | 250 million |
| Message length field | 2^62 (far above any real message; larger is refused) |
| Forecast step read from an `.idx` line | 0 to 1,000,000 hours (otherwise `valid_time` is NULL) |

Grid definitions are validated before use. Non-positive spacing, non-finite coordinates or earth radius, and grids over
the limit are rejected rather than producing incorrect or non-finite coordinates.

## Input caveats

- **Untrusted input:** the `.idx` and section-header parsers are covered by sanitizer and fuzz runs in CI. ecCodes parses
  the full message and is not fuzzed here. Read files you trust, or run DuckDB where a crash is contained, when the source
  is not under your control. Malformed messages usually produce an error, but ecCodes could still crash.
- **URLs with query strings:** presigned URLs do not get a sidecar lookup because `.idx` is appended to the whole URL.
  The inventory is therefore not found and `search` is unavailable; use `message := ...` instead.
- **Files without an `.idx`:** the file is walked message by message. Each message must fit in the remainder of the file
  and be at least 20 bytes; a message whose length runs past the end is reported as truncated.
- **Empty message list:** `message := []` is an error. Leave `message` out to read every message.
- **Missing values:** with a bitmap, a cell whose decoded value exactly matches the message's missing-value marker (9999
  unless the message says otherwise) is returned as NULL.
- **Errors and cancellation:** file read errors preserve their DuckDB error type and name the file and message number.
  `read_grib2` decodes on one thread and checks for cancellation between messages.

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
