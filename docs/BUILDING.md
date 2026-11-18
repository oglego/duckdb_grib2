# Build and test

This guide covers local development builds. For SQL usage, see the [README](../README.md).

## Prerequisites

- Git with submodule support
- CMake and a C++17 compiler
- Python 3.10 or newer
- vcpkg, configured as described in the [vcpkg setup guide](https://vcpkg.io/en/getting-started)

Clone with submodules, or initialize them in an existing checkout:

```sh
git submodule update --init --recursive
```

Set `VCPKG_TOOLCHAIN_PATH` to your vcpkg CMake toolchain, then build:

```sh
export VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
make
```

The build produces the DuckDB shell, test runner, and extension under `build/release/`. Load a local unsigned build with:

```sh
./build/release/duckdb -unsigned \
  -c "LOAD './build/release/extension/grib2/grib2.duckdb_extension';" \
  -c "SELECT grib2_eccodes_version();"
```

## Remote files in a local build

The bundled DuckDB build does not include `httpfs`. To test HTTP or S3 paths, build `httpfs` as a local extra extension.
Create the git-ignored file `extension_config_local.cmake`:

```cmake
duckdb_extension_load(httpfs
    GIT_URL https://github.com/duckdb/duckdb-httpfs
    GIT_TAG a2485c0fd240cbddc0d467019f30201c9a46cf06
)
```

Then build and load `httpfs` before `grib2`:

```sh
make EXTRA_EXTENSION_CONFIGS=$PWD/extension_config_local.cmake
./build/release/duckdb -unsigned \
  -c "LOAD './build/release/extension/httpfs/httpfs.duckdb_extension';" \
  -c "LOAD './build/release/extension/grib2/grib2.duckdb_extension';"
```

Keep this local config out of `extension_config.cmake`: CI builds extensions listed there, and `httpfs` has dependencies
that are not part of this project's vcpkg manifest.

## Tests and formatting

Run the SQL tests from the repository root:

```sh
make test
```

The test runner needs Python 3.10 or newer. If your Python is older, run the built test binary directly:

```sh
./build/release/test/unittest "test/*"
```

Standalone checks for the grid, `.idx` parser, and GRIB2 section parser are documented in [test/README.md](../test/README.md).
Before submitting changes, run `make format-check`; use `make format-fix` to apply formatting.
