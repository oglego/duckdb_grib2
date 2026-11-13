# Building from source

There is no published release yet. This guide covers building and testing the extension locally. For SQL usage, start
with the [README](../README.md).

## Prerequisites

DuckDB extensions use vcpkg for dependencies; this extension needs ecCodes. Follow the
[vcpkg installation instructions](https://vcpkg.io/en/getting-started) or run:

```sh
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Clone this project with submodules (`git clone --recurse-submodules`, or `git submodule update --init --recursive`), then
build:

```sh
make
```

This builds `./build/release/duckdb` (a DuckDB shell), `./build/release/test/unittest` (the test runner), and
`./build/release/extension/grib2/grib2.duckdb_extension` (the loadable extension). A locally built extension is unsigned
and is not loaded automatically:

```sh
./build/release/duckdb -unsigned \
  -c "LOAD './build/release/extension/grib2/grib2.duckdb_extension';" \
  -c "SELECT grib2_eccodes_version();"
```

## Build notes

- `python3` must be on your `PATH` for the ecCodes build.
- `vcpkg_ports/eccodes/` is a copy of the upstream vcpkg `eccodes` 2.47.0 port (from the vcpkg commit the DuckDB CI uses)
  with one change: the host `python3` dependency is removed. That dependency pulls in `python3[extensions]` and `libb2` on
  non-Windows platforms, and `libb2` needs `autoconf-archive` and `automake`, which the Linux CI images do not provide. The
  portfile still finds a system `python3`. When bumping the vcpkg commit, refresh the copy from upstream and reapply the
  change. `vcpkg.json` lists `./vcpkg_ports` ahead of `./extension-ci-tools/vcpkg_ports` under `overlay-ports`.
- **Platforms:** it builds on the default CI platforms, including Windows with MSVC. These are excluded
  (`exclude_archs` in `.github/workflows/MainDistributionPipeline.yml`): `wasm_mvp`, `wasm_eh`, and `wasm_threads` (ecCodes
  does not configure on the community `wasm32-emscripten` triplet) and `windows_amd64_mingw` (ecCodes fails to configure
  on `x64-mingw-static`).
- **`httpfs` in a local binary:** a binary built from the `duckdb` submodule has no published `httpfs`, so `INSTALL httpfs`
  returns a 404. Build it in with a local, git-ignored config. Do **not** add it to `extension_config.cmake`: CI builds
  every extension listed there, and `httpfs` needs libcurl, which is not in this repo's vcpkg manifest. Create
  `extension_config_local.cmake`:

  ```cmake
  duckdb_extension_load(httpfs
      GIT_URL https://github.com/duckdb/duckdb-httpfs
      GIT_TAG a2485c0fd240cbddc0d467019f30201c9a46cf06
  )
  ```

  Then run `make EXTRA_EXTENSION_CONFIGS=$PWD/extension_config_local.cmake` (tested on macOS arm64 with the system
  libcurl), and load both extensions by path, `httpfs` first:

  ```sh
  ./build/release/duckdb -unsigned \
    -c "LOAD './build/release/extension/httpfs/httpfs.duckdb_extension';" \
    -c "LOAD './build/release/extension/grib2/grib2.duckdb_extension';"
  ```

## Tests and formatting

Run the SQL tests with:

```sh
make test
```

`make test` runs DuckDB's `run_tests.py`, which needs Python 3.10 or newer. On older Pythons run the binary directly:
`./build/release/test/unittest "test/*"`. Run from the repository root because tests read their data files relative to
it.

The grid geometry and the `.idx` and section parsers also have standalone checks that need no DuckDB or ecCodes, run in
CI:

```sh
g++ -std=c++17 -O2 -I src/include test/cpp/grid_check.cpp -o /tmp/grid_check && /tmp/grid_check
g++ -std=c++17 -O2 -I src/include test/cpp/idx_check.cpp -o /tmp/idx_check && /tmp/idx_check
```

Test data and coverage are described in [`test/README.md`](../test/README.md). Before pushing, run `make format-check`
(or `make format-fix`). CI runs the format check only: the `tidy` check configures the project without vcpkg, so it cannot
find ecCodes.
