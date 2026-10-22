# duckdb_grib2

A DuckDB extension for querying GRIB2 weather data with SQL, powered by [ecCodes](https://github.com/ecmwf/eccodes).
The end goal is to run queries directly against GRIB2 files in object storage, such as the
[NOAA National Blend of Models (NBM)](https://registry.opendata.aws/noaa-nbm/) open dataset on S3.

> **Status: early development.** Only a smoke-test function exists today. The table functions below are planned.

Built from the [DuckDB extension template](https://github.com/duckdb/extension-template).

## Planned API

```sql
INSTALL grib2 FROM community;
LOAD grib2;

-- Catalog of messages in one or more files (one row per GRIB message)
SELECT * FROM grib2_index('s3://noaa-nbm-grib2-pds/blend.20260101/00/core/blend.t00z.core.f001.co.grib2');

-- Decoded grid values, with pruning by variable / level / step / bounding box
SELECT valid_time, lat, lon, value
FROM read_grib2(
    's3://noaa-nbm-grib2-pds/blend.20260101/00/core/*.grib2',
    variable := 'TMP',
    level := '2 m above ground',
    bbox := [-95.0, 36.0, -92.0, 38.0]
);
```

These signatures are a design sketch and may change.

## Available today

```sql
LOAD grib2;
SELECT grib2_eccodes_version();  -- version of the linked ecCodes library
```

## Roadmap

1. Decode spike: read a message from an in-memory buffer, resolve the ecCodes definitions path
2. `grib2_index()` (header scan, then `.idx` sidecar fast path)
3. `read_grib2()` with `variable`, `level`, `step`, `bbox`
4. Filter and projection pushdown
5. Parallel scans, tests, community extension submission

## Building
### Managing dependencies
DuckDB extensions use VCPKG for dependency management (this extension depends on ecCodes). Follow the [installation instructions](https://vcpkg.io/en/getting-started) or run the following:
```shell
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_TOOLCHAIN_PATH=`pwd`/vcpkg/scripts/buildsystems/vcpkg.cmake
```

### Build steps
Clone with submodules (`git clone --recurse-submodules`, or `git submodule update --init --recursive`), then run:
```sh
make
```
The main binaries that will be built are:
```sh
./build/release/duckdb
./build/release/test/unittest
./build/release/extension/grib2/grib2.duckdb_extension
```
- `duckdb` is the DuckDB shell with the extension code automatically loaded.
- `unittest` is the test runner of DuckDB. Again, the extension is already linked into the binary.
- `grib2.duckdb_extension` is the loadable binary as it would be distributed.

## Running the extension
Start the shell with `./build/release/duckdb`, then:
```
D select grib2_eccodes_version();
```

## Running the tests
SQL tests live in `./test/sql` and run with:
```sh
make test
```

### Installing the deployed binaries
To install your extension binaries from S3, you will need to do two things. Firstly, DuckDB should be launched with the
`allow_unsigned_extensions` option set to true. How to set this will depend on the client you're using. Some examples:

CLI:
```shell
duckdb -unsigned
```

Python:
```python
con = duckdb.connect(':memory:', config={'allow_unsigned_extensions' : 'true'})
```

NodeJS:
```js
db = new duckdb.Database(':memory:', {"allow_unsigned_extensions": "true"});
```

Secondly, you will need to set the repository endpoint in DuckDB to the HTTP url of your bucket + version of the extension
you want to install. To do this run the following SQL query in DuckDB:
```sql
SET custom_extension_repository='bucket.s3.eu-west-1.amazonaws.com/grib2/latest';
```
Note that the `/latest` path will allow you to install the latest extension version available for your current version of
DuckDB. To specify a specific version, you can pass the version instead.

After running these steps, you can install and load your extension using the regular INSTALL/LOAD commands in DuckDB:
```sql
INSTALL grib2;
LOAD grib2;
```

## Setting up CLion

### Opening project
Configuring CLion with this extension requires a little work. Firstly, make sure that the DuckDB submodule is available.
Then make sure to open `./duckdb/CMakeLists.txt` (so not the top level `CMakeLists.txt` file from this repo) as a project in CLion.
Now to fix your project path go to `tools->CMake->Change Project Root`([docs](https://www.jetbrains.com/help/clion/change-project-root-directory.html)) to set the project root to the root dir of this repo.

### Debugging
To set up debugging in CLion, there are two simple steps required. Firstly, in `CLion -> Settings / Preferences -> Build, Execution, Deploy -> CMake` you will need to add the desired builds (e.g. Debug, Release, RelDebug, etc). There's different ways to configure this, but the easiest is to leave all empty, except the `build path`, which needs to be set to `../build/{build type}`, and CMake Options to which the following flag should be added, with the path to the extension CMakeList:

```
-DDUCKDB_EXTENSION_CONFIGS=<path_to_the_exentension_CMakeLists.txt>
```

The second step is to configure the unittest runner as a run/debug configuration. To do this, go to `Run -> Edit Configurations` and click `+ -> Cmake Application`. The target and executable should be `unittest`. This will run all the DuckDB tests. To specify only running the extension specific tests, add `--test-dir ../../.. [sql]` to the `Program Arguments`. Note that it is recommended to use the `unittest` executable for testing/development within CLion. The actual DuckDB CLI currently does not reliably work as a run target in CLion.
