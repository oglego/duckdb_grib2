# Operations and limits

This page describes the runtime limits, remote reads, `.idx` rules and input caveats. For which grids, packing templates,
data sources and features are supported, see the [support matrix](SUPPORT.md). For SQL examples, see the
[README](../README.md).

## Verified data and formats

The verified inputs are NBM forecast-hour-1 core products for CONUS, Alaska, Hawaii, Puerto Rico, and Guam, and the HRRR
CONUS surface file `hrrr.t{HH}z.wrfsfcf00.grib2`. Other GRIB2 files may work, but have not been qualified. Anything
outside the [support matrix](SUPPORT.md) is refused with an error; `grib2_index` can still list the messages, but decoding
them is refused.

The extension accepts NOAA-style wgrib2 `.idx` inventories. Entries such as `n.m`, which represent several fields in one
message, are not supported. The reference time (`d=`) must be digits forming a real calendar date, and numbers must not
carry leading spaces or a `+`; an inventory that breaks these rules is refused with the line number. If no sidecar is available, the file is scanned message by message; `search := ...` requires
an available `.idx` inventory.

## Runtime settings

```sql
SET grib2_io_threads = 8;
```

The default is 1; the allowed range is 1–64. Values above 1 overlap file reads for `grib2_index` and prefetch message
bytes for `read_grib2` (up to six messages). Decoding remains single-threaded and row order stays deterministic. Parallel
file access has been exercised with HTTPS and public S3, but not every filesystem extension or DuckDB secret setup.
Queries that stop early may wait for reads already in flight.

## Concurrency

DuckDB can call the extension from several threads at once: the scalar functions `grib2_message_info` and
`grib2_point_value` run in parallel inside one query, and separate connections can run `read_grib2` and `grib2_index`
at the same time. ecCodes is only safe to use that way when it was built with thread support (its `ECCODES_THREADS` or
`ECCODES_OMP_THREADS` options, both off by default). The vcpkg build of this extension turns one of them on.

The extension asks the linked ecCodes how it was built. If it reports thread support, ecCodes calls run in parallel; if
it does not, or cannot say (ecCodes older than 2.47 cannot), the extension takes one process-wide lock around each use
of ecCodes so that only one thread is inside it at a time. Reading files and choosing cells stay outside the lock. This
matters if you build the extension against a distribution's ecCodes; it costs nothing with the bundled build.

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

A forecast step outside the supported range is left unset, and `valid_time` then comes from the message itself. For an `a-b hour` interval field
such as an accumulation, the step is the end of the interval; see [Forecast steps](REFERENCE.md#forecast-steps). Grid dimensions, spacing, coordinates,
and earth radius are checked before projection math is used.

## Remote files and the `.idx` sidecar

For each file the extension first tries to open `<file>.idx`. A server that answers 404 means there is none. S3 answers
403 instead of 404 for a key that does not exist when the caller may not list the bucket, which is how most private
buckets are set up, so a 403 on the sidecar is also read as "no `.idx`": the file is walked message by message and the
inventory columns are NULL. Any other failure on the sidecar, including 401 (wrong credentials), is reported as an error.
A sidecar that exists but that you may not read is therefore indistinguishable from a missing one; if the inventory
columns are unexpectedly NULL on a remote file, check the access policy for the `.idx` object.

How much of a sidecar is checked against the file depends on the call:

| Call | What is verified against the message headers |
|---|---|
| `grib2_index(file)` | The first and the last message. The last message's length is the rest of the file, so this catches a sidecar written for another version of the file, or a file that has grown or shrunk. The rows in between are listed as the `.idx` gives them, so one wrong offset in the middle is not noticed. Two small reads. |
| `grib2_index(file, details := true)` | Every message: its length must match what the `.idx` implies. |
| `read_grib2` | Every message it reads, before decoding it. |

Use `details := true` to audit a sidecar, or whenever a wrong inventory would matter more than the extra reads (one per
message, which adds up on a remote file).

## Missing values

Cells marked missing by a bitmap or by complex packing are returned with a SQL `NULL`. The decoder configures ecCodes to
use NaN as its missing marker, which preserves a legitimate value of 9999.

## Untrusted input

**Use this extension on files you trust.** It is built for archives from sources such as NOAA, not for files uploaded
by strangers. If you must read files you do not control, run the query in a separate, sandboxed process.

The `.idx` and section-header parsers are fuzzed and sanitizer-checked in CI. Before a complete message reaches ecCodes,
the extension validates its sections, counts, and supported packing layout. However, ecCodes still parses the full
message, and its decoder is mutation-tested against the CI distribution's ecCodes rather than the exact version built
into this extension. Malformed inputs should generally produce an error, but a decoder crash cannot be ruled out; use
trusted data when possible.

For a URL with a query string, the extension appends `.idx` to the full URL, so a sidecar is generally not found and
`search` is unavailable. Use `message := ...` to select by message number instead. Files without an `.idx` are scanned
from the beginning. A truncated message, an empty file, and bytes left over after the last message (even one stray
newline) are all errors, so a failed download cannot pass as a file with no data.

## Performance expectations

Every selected message is decoded in full, even when the query requests one point or a small box. Spatial filters reduce
the number of output rows, not the decode work. Remote reads can overlap when `grib2_io_threads` is greater than 1; value
decoding remains single-threaded.
