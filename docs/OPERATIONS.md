# Supported inputs and operational limits

This note expands on the support summary in the [README](../README.md#supported-data). Support is intentionally limited to
the combinations tested here; other GRIB2 data may work but has not been qualified.

## Supported inputs

- **Grids:** Lambert conformal (NBM CONUS), north polar stereographic (NBM Alaska), and Mercator (NBM Hawaii, Puerto Rico,
  Guam), on a spherical earth, with the usual scanning mode (rows stored west to east and south to north, with or without
  alternating row direction). Other grid types (for example regular latitude/longitude, as in GFS), ellipsoidal earths,
  south polar grids, and other scanning modes are not supported. A Lambert grid whose standard parallels do not define
  a cone (mirror images of each other, such as 30 and -30 degrees, or both on the equator) is refused with an error.
- **One field per message:** a GRIB2 message may repeat sections 4 to 7 to carry several fields. ecCodes decodes only the
  first, so `read_grib2`, `grib2_message_info` and `grib2_point_value` refuse such a message with an error ("more than
  one field") instead of silently dropping the rest. NOAA's NBM and HRRR files hold one field per message.
- **Packing:** ecCodes decodes the values, but only for packing templates 5.0 (simple), 5.2 (complex) and 5.3 (complex with
  spatial differencing). NBM uses 5.2 and 5.3 and HRRR uses 5.0 and 5.3, and the extension checks all three layouts in
  depth before ecCodes sees the message. A message with any other template (5.40 JPEG 2000, 5.42 CCSDS, ...) is refused by `read_grib2`,
  `grib2_message_info` and `grib2_point_value` with an error naming the template. `grib2_index` still lists such
  messages and reports their `packing_template`, so you can see what a file holds. JPEG 2000 support (OpenJPEG) is
  linked into ecCodes but not reachable through the extension; CCSDS is not built in.
- **Inventory files:** the NOAA-published wgrib2-style `.idx` format. Lines of the form `n.m` (several fields in one
  message) are not supported yet.
- **Current verified data:** NBM CONUS, Alaska, Hawaii, Puerto Rico, and Guam (forecast hour 1 of the core product), and
  the HRRR CONUS surface file `s3://noaa-hrrr-bdp-pds/hrrr.{YYYYMMDD}/conus/hrrr.t{HH}z.wrfsfcf00.grib2`. This is the
  tested matrix in this repository. For HRRR it consists of three real messages (5.0 constant field, 5.3 with 2 and 17 bits
  per value) checked against ecCodes, and the HRRR grid (Lambert, earth shape 6, rows not alternating) in the projection
  oracle. Simple packing with varying values, a bitmap, and real 9999 values is tested on windows of the real grid
  re-encoded as 5.0 (`test/data/simple_*.grib2`; synthetic re-encodings, not NOAA output). Not yet covered: a real NOAA 5.0
  message with varying values, other HRRR products and domains, and other sources.

## Settings and limits

```sql
SET grib2_io_threads = 8;   -- default 1, range 1 to 64
```

By default every file read happens on the query's own thread. Setting `grib2_io_threads` above 1 opts in to overlapping
network round trips: `grib2_index` indexes several files at once and `read_grib2` keeps up to that many message fetches
(at most 6) in flight while it decodes the current message. This is much faster for remote files, and the results are
identical.

The overlap is opt-in because those background threads call DuckDB's file system from outside DuckDB's own scheduler.
That has been tested with plain `https://` and public `s3://` reads, but not with every filesystem extension or with
credentials supplied through DuckDB secrets, so try it on your setup before relying on it. A query that ends early (for
example with `LIMIT`) waits for fetches already started on background threads. Queries are checked for cancellation
between messages in both functions.

Hard resource and input limits are reported as errors naming the file and message where applicable. A forecast step
outside the supported range is left unset, so `valid_time` is NULL:

| Limit | Value |
|---|---|
| `.idx` file size | 64 MiB |
| One message | 256 MiB |
| Grid size (`Ni x Nj`) | 64 million cells |
| `search` pattern length | 1024 characters |
| Message bytes held in flight by prefetching | 128 MiB (at least one message) |
| Packing templates decoded | 5.0, 5.2 and 5.3 only |
| Grid or data points a message may claim (checked before ecCodes sees it) | 64 million |
| Message headers (sections 0 to 5) read by `grib2_index` and `read_grib2` | 1 MiB; a message with larger headers is an error, refused before it is read |
| Message prefix given to ecCodes by `grib2_index(..., details := true)` | the same headers, and only if the message is within the point-count limit |
| Message length field | 2^62 (far above any real message; larger is refused) |
| Forecast step read from an `.idx` line | 0 to 1,000,000 hours (otherwise `valid_time` is NULL) |

Grid definitions are validated before use. Non-positive spacing, non-finite coordinates or earth radius, and grids over
the limit are rejected rather than producing incorrect or non-finite coordinates.

## Input caveats

- **Untrusted input:** the `.idx` and section-header parsers are covered by sanitizer and fuzz runs in CI, and only the
  three packing templates the extension validates in depth (5.0, 5.2, 5.3) are passed to ecCodes for decoding.
  (`grib2_index(..., details := true)` gives ecCodes a short prefix, sections 0 to 5, to read names and grid keys. Headers
  over 1 MiB are refused before they are read; a message that claims more points than the limit below gets no details.) ecCodes parses
  the full message. Its decoder is mutation-tested in CI against the distribution's ecCodes, not the version built into
  the extension, so it may still crash on malformed input. Read files you trust, or run DuckDB where a crash is contained, when the source
  is not under your control. Malformed messages usually produce an error, but ecCodes could still crash.
- **URLs with query strings:** presigned URLs do not get a sidecar lookup because `.idx` is appended to the whole URL.
  The inventory is therefore not found and `search` is unavailable; use `message := ...` instead.
- **Files without an `.idx`:** the file is walked message by message. Each message must fit in the remainder of the file
  and be at least 20 bytes; a message whose length runs past the end is reported as truncated.
- **Empty message list:** `message := []` is an error. Leave `message` out to read every message.
- **Missing values:** a cell with no data is returned as NULL, whether a bitmap masks it or complex packing (5.2, 5.3)
  flags it as missing. The extension has ecCodes decode such cells as NaN rather than its default 9999, so a real value of
  exactly 9999 is kept.
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
