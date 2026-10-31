# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(grib2
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)
#
# Do not add httpfs here: CI builds every extension listed in this file, and httpfs needs libcurl, which is not in this
# repository's vcpkg manifest. For local testing against remote files, see "Remote files" in the README.
