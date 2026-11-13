# How the extension uses ecCodes

This note describes the boundary between the extension's own GRIB2 handling and ecCodes. It is intended as a
code-oriented companion to the SQL reference in the [README](../README.md).

## What ecCodes does

The extension links the ecCodes C library. The repository's vcpkg port pins ecCodes 2.47.0; it also includes the
OpenJPEG dependency used for JPEG 2000 support. The extension does not shell out to `grib_get`, `grib_dump`, or other
ecCodes command-line tools, and it does not use Python bindings at query time.

For `read_grib2`, ecCodes is responsible for:

1. Constructing a handle for each complete selected GRIB2 message.
2. Reading the grid definition and selected metadata keys through the ecCodes key interface.
3. Decoding the packed data into the message's `values` array.
4. Providing the message's `shortName` and bitmap/missing-value metadata.

The extension handles file discovery, `.idx` parsing, GRIB2 section-header scanning, message selection, grid projection
math, point and box selection, value-to-grid ordering, and DuckDB output rows itself. In particular, point lookup and
coordinate generation do not call ecCodes' coordinate-array or nearest-point routines.

## `read_grib2` data path

The implementation is in [`grib2_read_function.cpp`](../src/grib2_read_function.cpp). Its work is split between setup,
message loading, and row production:

1. **Find messages.** DuckDB's file system expands the input path or glob and the extension sorts paths. The shared
   indexer reads a neighboring `.idx` when available; otherwise, it walks the GRIB2 indicator and section headers.
   Those offsets, lengths, reference times, and any inventory text are parsed by extension code, not ecCodes.
   `message :=` filters by message number. `search :=` applies DuckDB's RE2 regular expression to the inventory text
   and therefore requires a usable `.idx`.
2. **Fetch selected message bytes.** The reader range-reads each selected message using the index offsets. It checks
   that the bytes contain a complete GRIB2 message whose length agrees with the index before passing them to ecCodes.
   With `grib2_io_threads` above 1, upcoming message reads may run asynchronously (up to six in flight, with a 128 MiB
   byte budget); ecCodes decoding and row emission remain on the query thread and preserve file/message order.
3. **Create and inspect an ecCodes handle.** `OpenGribMessage` calls
   `codes_handle_new_from_message_copy`. The copy gives the handle its own message buffer. A small pre-check and
   section/data-representation checks are performed first to reject obvious non-GRIB input and several malformed or
   unreasonably large declarations before ecCodes parses the full message.
4. **Build the supported grid.** `MakeGrid` reads the grid keys listed below, validates them, and constructs the
   extension's spherical Lambert conformal, Mercator, or north polar-stereographic projection. Other grid types,
   scanning arrangements, Earth shapes, and south-polar stereographic grids are rejected as unsupported.
5. **Decode all data values.** The extension asks ecCodes for the size of the `values` key with
   `codes_get_size(handle, "values", ...)`, then decodes that array with
   `codes_get_double_array(handle, "values", ...)`. The entire message is decoded even for a point or small box;
   spatial selection reduces output, not decode work.
6. **Select cells and map values.** The extension's own grid code calculates cell coordinates and chooses the nearest
   cell(s), cells inside a bounding box, or every cell. For alternating row scanning, `RawValueIndex` maps a canonical
   `(i, j)` cell to the stored position in ecCodes' `values` array. The decoded value count must equal `Ni * Nj`.
7. **Emit rows.** DuckDB receives one row per selected cell per message. A bitmap-marked cell whose value equals the
   message's `missingValue` is emitted with a NULL `value`; points outside the grid emit no row.

## ecCodes keys used by `read_grib2`

The following are the keys read directly through the ecCodes C API in the `read_grib2` path. “Required” means a missing
or unreadable key produces an error; optional keys use the noted fallback.

| Key(s) | Use |
|---|---|
| `values` | Decoded data array; its length is obtained first, then all values are read as doubles. |
| `gridType` | Selects `lambert`, `mercator`, or `polar_stereographic`; other types are unsupported. |
| `shapeOfTheEarth`, `radius` | Validates and configures the spherical-earth projection. Only shape codes 0, 1, 6, and 8 are accepted by the current code. |
| `Ni`, `Nj` | Grid dimensions and the expected decoded value count. |
| `latitudeOfFirstGridPointInDegrees`, `longitudeOfFirstGridPointInDegrees` | Projection origin / first grid cell. |
| `LaDInDegrees`, `DxInMetres`, `DyInMetres` | Projection true latitude and grid spacing. |
| `iScansNegatively`, `jScansPositively`, `jPointsAreConsecutive` | Checks that the scanning mode matches the value-index mapping. Required values are 0, 1, and 0, respectively. |
| `alternativeRowScanning` | Whether alternate rows are reversed in the stored value array; defaults to 0 if unavailable. |
| `LoVInDegrees`, `Latin1InDegrees`, `Latin2InDegrees` | Lambert conformal orientation and standard parallels. Required for Lambert grids. |
| `southPoleOnProjectionPlane`, `orientationOfTheGridInDegrees` | Rejects south-polar grids and configures north polar stereographic orientation. Both are required for that grid type. |
| `bitmapPresent` | Identifies messages with a bitmap; defaults to 0 if unavailable. |
| `missingValue` | Identifies the bitmap's decoded missing marker; defaults to 9999 if unavailable. |
| `shortName` | Populates the output `short_name`; unavailable keys result in NULL. |

`grib2_index` metadata such as `variable`, `level`, `forecast`, and `qualifier` comes from the NOAA-style `.idx` text
when present, not from `read_grib2`'s ecCodes handle. Message reference time and parameter/grid/packing template numbers
can also be read by the extension's section parser when it walks messages without a sidecar. ecCodes is additionally
used for names and grid details by `grib2_index(..., details := true)`, but that is a separate optional index mode.

## Other ecCodes calls exposed by the extension

- `grib2_eccodes_version()` calls `grib_get_api_version()` and formats the linked library version.
- `grib2_message_info(BLOB)` opens the first message with the same copied-message API, decodes `values`, and reports
  selected keys including `edition`, `shortName`, `typeOfLevel`, `level`, `gridType`, `Ni`, `Nj`, `numberOfPoints`,
  `packingType`, and `alternativeRowScanning`, plus non-bitmap min/max values.
- `grib2_point_value(BLOB, lat, lon)` uses the same handle, grid construction, complete `values` decode, stored-row
  mapping, and bitmap behavior as the reader, but returns only the nearest cell's value.
- `grib2_index(..., details := true)` uses `codes_handle_new_from_partial_message_copy` on a short message prefix to
  read `shortName`, `typeOfLevel`, `level`, `gridType`, `Ni`, `Nj`, and `alternativeRowScanning`. The extension's
  section parser supplies the packing template because a partial handle is not used for that field.

## Related implementation and tests

- ecCodes wrappers and the grid-key reader: [`grib2_codes.hpp`](../src/include/grib2_codes.hpp)
- Grid projection and value ordering: [`grib2_grid.hpp`](../src/include/grib2_grid.hpp)
- GRIB2 section parsing and pre-decode checks: [`grib2_message.hpp`](../src/include/grib2_message.hpp)
- SQL behavior tests: [`grib2.test`](../test/sql/grib2.test)
- Grid/value-order reference checks: [`grid_check.cpp`](../test/cpp/grid_check.cpp) and
  [`generate_grid_reference.py`](../test/data/generate_grid_reference.py)

The supported formats and operational limits are summarized in the README's [What it reads](../README.md#what-it-reads)
and [Settings, limits and untrusted input](../README.md#settings-limits-and-untrusted-input) sections. ecCodes performs
the full-message decode and is not fuzzed by this repository; see the README's untrusted-input guidance before reading
data from sources you do not trust.
