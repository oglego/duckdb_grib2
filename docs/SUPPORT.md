# What is supported, and what is not

The extension decodes only what it has screened and tested, and refuses everything else with an error (`grib2_index` can
still list the metadata of refused messages). This page is the single place to check whether your files will work. For
runtime settings, remote reads and `.idx` rules see [OPERATIONS](OPERATIONS.md); for what each column holds see
[REFERENCE](REFERENCE.md).

The tables follow the code: grids and scan order in `MakeGrid` (`src/include/grib2_codes.hpp`), packing and message rules
in `ScreenMessage` and `SCREENED_PACKING_TEMPLATES` (`src/include/grib2_message.hpp`). Change this page in the same commit
as either.

**Legend:** ✅ supported and tested · 🔒 refused on purpose (reason given) · ❌ not supported yet

## Grids (section 3)

| Grid | GRIB2 template | Status | Notes |
|---|---|---|---|
| Lambert conformal | 3.30 | ✅ | NBM CONUS, HRRR. |
| Polar stereographic, north pole | 3.20 | ✅ | NBM Alaska. LaD must be above -90°. |
| Mercator | 3.10 | ✅ | NBM Hawaii, Puerto Rico, Guam. Must span less than 180° of longitude. The orientation angle is ignored, as in ecCodes. |
| Regular latitude/longitude | 3.0 | ❌ | Used by GFS and many global files. The biggest gap for new users. |
| Gaussian | 3.40 | ❌ | ECMWF. |
| Rotated lat/lon, spherical harmonics, others | 3.1, 3.50, … | ❌ | |
| Polar stereographic, south pole | 3.20 | ❌ | |

| Earth shape | Status |
|---|---|
| Spherical (shape codes 0, 1, 6, 8) | ✅ |
| Oblate / ellipsoidal (2, 3, 4, 5, 7) | ❌ |

| Scan order | Status |
|---|---|
| West to east, south to north, rows stored consecutively, optionally with alternating row direction | ✅ |
| Any other scan order (including columns stored consecutively) | ❌ |

## Packing (section 5)

| Packing | Template | Status | Why |
|---|---|---|---|
| Simple | 5.0 | ✅ | NBM, HRRR. Layout check, real fixtures, Herbie comparison. |
| Complex | 5.2 | ✅ | NBM. |
| Complex with spatial differencing | 5.3 | ✅ | NBM, HRRR. |
| JPEG 2000 | 5.40 | 🔒 | ecCodes could decode it, but no layout check or verified fixture exists, and a malformed message would reach the decoder unchecked. |
| PNG | 5.41 | 🔒 | Same, and the build does not include libpng. |
| CCSDS / AEC | 5.42 | 🔒 | Same, and the build does not include libaec. Used by ECMWF open data. |
| Anything else (IEEE float, matrix, spectral, …) | 5.1, 5.4, 5.50, … | 🔒 | Not screened. |

Other message-level rules:

| Rule | Status |
|---|---|
| One field per message | ✅ (multi-field messages are refused, as is a message whose sections 5, 6, 7 are out of order or followed by another section) |
| No bitmap (indicator 255) or an explicit bitmap (indicator 0) | ✅ |
| Reuse-previous or predefined bitmaps (indicators 1 to 254) | 🔒 refused: nothing in a one-field message for them to refer to |
| Complex-packed group lengths that do not add up to the value count | 🔒 refused as corrupt |
| Up to 64 million grid cells per message | ✅ (larger is refused) |
| Product templates (section 4) | ✅ 4.0 (instantaneous) and 4.8 (interval) are tested. Others pass through to ecCodes untested, so check `valid_time` before relying on it. |

## Data verified against Herbie / ecCodes

| Source | Status |
|---|---|
| NOAA NBM forecast hour 1 core: CONUS, Alaska, Hawaii, Puerto Rico, Guam | ✅ |
| NOAA HRRR CONUS surface file | ✅ |
| Other NOAA products, other forecast hours | ⚠️ expected to work if grid and packing are in the tables above, but not verified |
| GFS, ECMWF, other centres | ❌ blocked by grid type (and, for ECMWF, packing) |

## Features

| Feature | Status | Notes |
|---|---|---|
| Nearest-cell lookup for points | ✅ | |
| Bounding-box selection (incl. across the antimeridian) | ✅ | A box with `lon_max < lon_min` needs `wrap := true`. |
| Every cell of a grid (`all_cells`) | ✅ | About 27 s for 324M rows. |
| Missing values as SQL `NULL` | ✅ | NaN is used internally, so a real value of 9999 survives. |
| NOAA `.idx` inventories (`search`, `message`) | ✅ | `n.m` entries are not supported. |
| Files without an `.idx` | ✅ | Walked message by message. Empty files, truncated messages and trailing bytes are errors. |
| Local files, `http(s)://`, `s3://` (via httpfs) | ✅ | A 403 on a missing `.idx` is read as "no sidecar". |
| Background prefetch (`grib2_io_threads`) | ✅ | |
| `valid_time` | ✅ | From the `.idx` step when it has one, otherwise from the message. NULL when the reference time is not a run time (significance other than 0 or 1). In `read_grib2` and, with `details := true`, in `grib2_index`. |
| Scalar functions on BLOBs (`grib2_message_info`, `grib2_point_value`) | ✅ | |
| Parallel decoding of one message | ❌ | Decoding is single-threaded. |
| Column or filter pushdown | ❌ | All columns are produced even for `count(*)`. |
| Skip decoding when no cells are selected | ❌ | A message is fully decoded even if a point falls outside the grid. |

## Platforms

| Platform | Status |
|---|---|
| Linux, macOS, Windows (MSVC) | targeted by the distribution pipeline (confirm a green build before release) |
| WebAssembly, Windows MinGW | ❌ excluded |

## Adding a packing template or grid

A template is added only with all of these, so the allowed set only grows when it is safe to:

1. A layout check in `CheckDataRepresentation` (declared counts and widths against the size of the data section).
2. A real-data fixture in `fixtures/`, with its source recorded in `fixtures/README.md`.
3. Corrupt-input tests (`robustness_check`, `data_repr_check`).
4. A comparison against Herbie on every cell (`test/herbie_compare`).
5. For a grid: a projection checked against ecCodes' own coordinates (`grid_check`).
