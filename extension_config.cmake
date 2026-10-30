# This file is included by DuckDB's build system. It specifies which extension to load

# Extension from this repo
duckdb_extension_load(grib2
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)

duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG a2485c0fd240cbddc0d467019f30201c9a46cf06
)

# Any extra extensions that should be built
# e.g.: duckdb_extension_load(json)