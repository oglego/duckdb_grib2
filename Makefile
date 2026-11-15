PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=grib2
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Only test/sql holds tests for the runner. The default is all of test/, which also reaches into a local Python
# virtualenv (test/herbie_compare/.venv has a bin/py.test, which the runner mistakes for a test file).
TESTS_BASE_DIRECTORY ?= test/sql/

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile