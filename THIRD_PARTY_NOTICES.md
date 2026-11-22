# Third-party notices

The extension binary includes the following libraries, built through vcpkg. Each is used under its own license; the
license texts ship with the sources vcpkg downloads and are available in the upstream repositories.

| Library | Use | License | Source |
|---|---|---|---|
| ecCodes | Decodes GRIB messages and provides the definition tables (embedded in the library at build time) | Apache License 2.0 | https://github.com/ecmwf/eccodes |
| OpenJPEG | JPEG 2000 support for ecCodes | BSD 2-Clause | https://github.com/uclouvain/openjpeg |

The extension itself is under the MIT License (see `LICENSE`). It is loaded into DuckDB, which is not part of the extension
binary, and it uses DuckDB's own copy of RE2 for the `search` option, so neither is bundled here.

## Test data

The GRIB2 messages in `fixtures/` are unmodified messages from two NOAA open datasets, extracted by byte range. See
`fixtures/README.md` for the exact source of each.

| Files | Source | Dataset and terms of use |
|---|---|---|
| `fixtures/nbm_*.grib2` and the `.idx` there | NOAA National Blend of Models | https://registry.opendata.aws/noaa-nbm/ |
| `fixtures/hrrr_*.grib2` | NOAA High-Resolution Rapid Refresh | https://registry.opendata.aws/noaa-hrrr-pds/ |

Many files in `test/data/` are derived from those messages: re-encoded, windowed, masked or concatenated, or with a
synthetic `.idx` inventory. The rest are generated from scratch. Each is produced by the script beside it, and
`test/README.md` says which source each one comes from.
