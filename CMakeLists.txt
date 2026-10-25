cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME grib2)

# DuckDB's extension distribution supports vcpkg. Dependencies are declared in
# ./vcpkg.json and consumed here with find_package.
find_package(eccodes CONFIG REQUIRED)

# The ecCodes vcpkg port may export either `eccodes` or a namespaced
# `eccodes::eccodes` target depending on version; support both.
if(TARGET eccodes::eccodes)
  set(GRIB2_ECCODES_TARGET eccodes::eccodes)
elseif(TARGET eccodes)
  set(GRIB2_ECCODES_TARGET eccodes)
else()
  message(
    FATAL_ERROR
      "ecCodes was found but exports neither `eccodes` nor `eccodes::eccodes`")
endif()

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

set(EXTENSION_SOURCES src/grib2_extension.cpp src/grib2_index_function.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Link ecCodes in both the static library and the loadable extension
target_link_libraries(${EXTENSION_NAME} ${GRIB2_ECCODES_TARGET})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${GRIB2_ECCODES_TARGET})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
