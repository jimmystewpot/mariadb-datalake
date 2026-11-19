# Copyright (c) 2026, MariaDB Foundation.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

#
# Build DuckDB static libraries from submodule source.
#
# The upstream DuckDB repo lives at storage/duckdb/third_parties/duckdb/
# as a git submodule.  We build it via ExternalProject so its CMake targets
# (including one named "duckdb") don't clash with the MariaDB plugin target
# of the same name created by MYSQL_ADD_PLUGIN().
#

SET(_MAIN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
IF(NOT EXISTS "${_MAIN_REPO_ROOT}/third_parties/duckdb/CMakeLists.txt" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../third_parties/duckdb/CMakeLists.txt")
  GET_FILENAME_COMPONENT(_MAIN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
ENDIF()

SET(DUCKDB_SUBMODULE_DIR "${_MAIN_REPO_ROOT}/third_parties/duckdb")
SET(DUCKDB_INCLUDE_DIR   "${DUCKDB_SUBMODULE_DIR}/src/include")

INCLUDE(ExternalProject)

# Map MariaDB build type to a DuckDB-friendly one.
IF(CMAKE_BUILD_TYPE MATCHES "[Dd]ebug")
  SET(_DUCKDB_BUILD_TYPE "Debug")
ELSE()
  SET(_DUCKDB_BUILD_TYPE "Release")
ENDIF()

IF(NOT "${_MAIN_REPO_ROOT}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}" AND
   EXISTS "${_MAIN_REPO_ROOT}/build/duckdb-build/src/libduckdb_static.a" AND
   EXISTS "${_MAIN_REPO_ROOT}/build/duckdb-deps-install/lib/libroaring.a")
  SET(_DUCKDB_BUILD_DIR "${_MAIN_REPO_ROOT}/build/duckdb-build")
  SET(_DUCKDB_DEPS_PREFIX "${_MAIN_REPO_ROOT}/build/duckdb-deps-install")
  SET(_DUCKDB_PREBUILT TRUE)
ELSE()
  SET(_DUCKDB_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/duckdb-build")
  SET(_DUCKDB_DEPS_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/duckdb-deps-install")
  SET(_DUCKDB_PREBUILT FALSE)
ENDIF()

# Statically linked DuckDB capabilities and extensions, in load order.
# Extensions must also be listed in cmake/duckdb_extensions.cmake.
# local_extension_repository is left out on purpose: it would redirect
# automatic installs to the DuckDB build tree.
SET(_DUCKDB_LINKED_EXTENSIONS
  httplib loadable_extensions core_functions icu json parquet quack)

# Remote file access (https, http, s3) in DuckDB. Needs OpenSSL and libcurl,
# linked from the system as mysqld links OpenSSL. Without them the engine is
# built without it, and remote reads are refused with an error saying so.
OPTION(WITH_DUCKDB_HTTPFS "Link DuckDB's httpfs extension" ON)
IF(WITH_DUCKDB_HTTPFS)
  FIND_PACKAGE(OpenSSL)
  FIND_PACKAGE(CURL)
  IF(OPENSSL_FOUND AND CURL_FOUND)
    LIST(APPEND _DUCKDB_LINKED_EXTENSIONS httpfs)
  ELSE()
    MESSAGE(WARNING "OpenSSL or libcurl not found: building without httpfs")
    SET(WITH_DUCKDB_HTTPFS OFF)
  ENDIF()
ENDIF()

OPTION(WITH_DUCKDB_ICEBERG "Link DuckDB's avro and iceberg extensions" ON)
IF(WITH_DUCKDB_ICEBERG)
  LIST(APPEND _DUCKDB_LINKED_EXTENSIONS avro iceberg)

  # 1. Build and install static CRoaring v4.2.1 for duckdb-iceberg
  FILE(MAKE_DIRECTORY
    "${_DUCKDB_DEPS_PREFIX}/include"
    "${_DUCKDB_DEPS_PREFIX}/lib"
    "${_DUCKDB_DEPS_PREFIX}/lib/cmake/roaring"
    "${_DUCKDB_DEPS_PREFIX}/lib/cmake/jansson")
  IF(NOT EXISTS "${_DUCKDB_DEPS_PREFIX}/include/roaring/roaring64.h")
    MESSAGE(STATUS "Building static CRoaring v4.2.1 (libroaring.a) for duckdb-iceberg...")
    SET(_ROARING_TARBALL "${CMAKE_CURRENT_BINARY_DIR}/CRoaring-4.2.1.tar.gz")
    SET(_ROARING_SRC_DIR "${CMAKE_CURRENT_BINARY_DIR}/CRoaring-4.2.1")
    SET(_ROARING_BLD_DIR "${CMAKE_CURRENT_BINARY_DIR}/CRoaring-4.2.1-build")
    IF(NOT EXISTS "${_ROARING_TARBALL}")
      FILE(DOWNLOAD
        "https://github.com/RoaringBitmap/CRoaring/archive/refs/tags/v4.2.1.tar.gz"
        "${_ROARING_TARBALL}"
        STATUS _roaring_dl_status)
      LIST(GET _roaring_dl_status 0 _roaring_dl_code)
      IF(NOT _roaring_dl_code EQUAL 0)
        MESSAGE(FATAL_ERROR "Failed to download CRoaring-4.2.1.tar.gz: ${_roaring_dl_status}")
      ENDIF()
    ENDIF()
    FILE(REMOVE_RECURSE "${_ROARING_SRC_DIR}" "${_ROARING_BLD_DIR}")
    FILE(MAKE_DIRECTORY "${_ROARING_BLD_DIR}")
    EXECUTE_PROCESS(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${_ROARING_TARBALL}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      RESULT_VARIABLE _roaring_tar_rc)
    IF(NOT _roaring_tar_rc EQUAL 0)
      MESSAGE(FATAL_ERROR "Failed to extract CRoaring-4.2.1.tar.gz")
    ENDIF()
    EXECUTE_PROCESS(
      COMMAND ${CMAKE_COMMAND} -E env CMAKE_POLICY_VERSION_MINIMUM=3.5
              ${CMAKE_COMMAND} -S "${_ROARING_SRC_DIR}" -B "${_ROARING_BLD_DIR}"
              -DCMAKE_BUILD_TYPE=Release
              -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
              -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
              -DCMAKE_POSITION_INDEPENDENT_CODE=ON
              "-DCMAKE_C_FLAGS=-fPIC -fvisibility=hidden"
              "-DCMAKE_CXX_FLAGS=-fPIC -fvisibility=hidden"
              -DCMAKE_INSTALL_PREFIX=${_DUCKDB_DEPS_PREFIX}
              -DCMAKE_INSTALL_LIBDIR=lib
              -DROARING_BUILD_STATIC=ON
              -DBUILD_SHARED_LIBS=OFF
              -DENABLE_ROARING_TESTS=OFF
              -DROARING_USE_CPM=OFF
      RESULT_VARIABLE _roaring_cfg_rc)
    IF(NOT _roaring_cfg_rc EQUAL 0)
      MESSAGE(FATAL_ERROR "Failed to configure CRoaring-4.2.1")
    ENDIF()
    EXECUTE_PROCESS(
      COMMAND ${CMAKE_COMMAND} --build "${_ROARING_BLD_DIR}" --target install -j4
      RESULT_VARIABLE _roaring_bld_rc)
    IF(NOT _roaring_bld_rc EQUAL 0)
      MESSAGE(FATAL_ERROR "Failed to build/install CRoaring-4.2.1")
    ENDIF()
  ENDIF()

  # 2. Build and install static libjansson.a for duckdb-avro (avro-c)
  IF(NOT EXISTS "${_DUCKDB_DEPS_PREFIX}/lib/libjansson.a")
    MESSAGE(STATUS "Building static libjansson.a for duckdb-avro...")
    SET(_JANSSON_TARBALL "${CMAKE_CURRENT_BINARY_DIR}/jansson-2.14.tar.gz")
    SET(_JANSSON_SRC_DIR "${CMAKE_CURRENT_BINARY_DIR}/jansson-2.14")
    SET(_JANSSON_BLD_DIR "${CMAKE_CURRENT_BINARY_DIR}/jansson-2.14-build")
    IF(NOT EXISTS "${_JANSSON_TARBALL}")
      FILE(DOWNLOAD
        "https://github.com/akheron/jansson/releases/download/v2.14/jansson-2.14.tar.gz"
        "${_JANSSON_TARBALL}"
        STATUS _dl_status)
      LIST(GET _dl_status 0 _dl_code)
      IF(NOT _dl_code EQUAL 0)
        MESSAGE(FATAL_ERROR "Failed to download jansson-2.14.tar.gz: ${_dl_status}")
      ENDIF()
    ENDIF()
    FILE(REMOVE_RECURSE "${_JANSSON_SRC_DIR}" "${_JANSSON_BLD_DIR}")
    FILE(MAKE_DIRECTORY "${_JANSSON_BLD_DIR}")
    EXECUTE_PROCESS(
      COMMAND ${CMAKE_COMMAND} -E tar xzf "${_JANSSON_TARBALL}"
      WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
      RESULT_VARIABLE _jansson_tar_rc)
    IF(NOT _jansson_tar_rc EQUAL 0)
      MESSAGE(FATAL_ERROR "Failed to extract jansson-2.14.tar.gz")
    ENDIF()
    EXECUTE_PROCESS(
      COMMAND ${CMAKE_COMMAND} -E env CMAKE_POLICY_VERSION_MINIMUM=3.5
              ${CMAKE_COMMAND} -S "${_JANSSON_SRC_DIR}" -B "${_JANSSON_BLD_DIR}"
              -DCMAKE_BUILD_TYPE=Release
              -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
              -DCMAKE_POSITION_INDEPENDENT_CODE=ON
              "-DCMAKE_C_FLAGS=-fPIC -fvisibility=hidden"
              -DCMAKE_INSTALL_PREFIX=${_DUCKDB_DEPS_PREFIX}
              -DCMAKE_INSTALL_LIBDIR=lib
              -DJANSSON_BUILD_DOCS=OFF
              -DJANSSON_BUILD_SHARED_LIBS=OFF
              -DJANSSON_WITHOUT_TESTS=ON
              -DJANSSON_EXAMPLES=OFF
              -DCMAKE_POLICY_VERSION_MINIMUM=3.5
      RESULT_VARIABLE _jansson_cfg_rc)
    IF(NOT _jansson_cfg_rc EQUAL 0)
      MESSAGE(FATAL_ERROR "Failed to configure jansson-2.14")
    ENDIF()
    EXECUTE_PROCESS(
      COMMAND ${CMAKE_COMMAND} --build "${_JANSSON_BLD_DIR}" --target install -j4
      RESULT_VARIABLE _jansson_bld_rc)
    IF(NOT _jansson_bld_rc EQUAL 0)
      MESSAGE(FATAL_ERROR "Failed to build/install jansson-2.14")
    ENDIF()
  ENDIF()
  FILE(WRITE "${_DUCKDB_DEPS_PREFIX}/lib/cmake/jansson/janssonConfig.cmake"
"if(NOT TARGET jansson::jansson)
  add_library(jansson::jansson STATIC IMPORTED)
  set_target_properties(jansson::jansson PROPERTIES
    IMPORTED_LOCATION \"${_DUCKDB_DEPS_PREFIX}/lib/libjansson.a\"
    INTERFACE_INCLUDE_DIRECTORIES \"${_DUCKDB_DEPS_PREFIX}/include\")
endif()
set(JANSSON_INCLUDE_DIRS \"${_DUCKDB_DEPS_PREFIX}/include\")
set(JANSSON_LIBRARY_DIRS \"${_DUCKDB_DEPS_PREFIX}/lib\")
set(JANSSON_LIBRARIES jansson::jansson)
set(jansson_FOUND TRUE)
")
ENDIF()

SET(_DUCKDB_STATIC_LIBS
  "${_DUCKDB_BUILD_DIR}/src/main/http/libduckdb_httplib.a"
  "${_DUCKDB_BUILD_DIR}/src/main/extension/libduckdb_loadable_extensions.a"
  "${_DUCKDB_BUILD_DIR}/extension/core_functions/libcore_functions_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/icu/libicu_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/json/libjson_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/parquet/libparquet_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/quack/libquack_extension.a"
  "${_DUCKDB_BUILD_DIR}/src/libduckdb_static.a"
)
IF(WITH_DUCKDB_ICEBERG)
  LIST(APPEND _DUCKDB_STATIC_LIBS
    "${_DUCKDB_BUILD_DIR}/extension/iceberg/libiceberg_extension.a"
    "${_DUCKDB_BUILD_DIR}/extension/avro/libavro_extension.a"
    "${_DUCKDB_BUILD_DIR}/extension/avro/third_party/avro-c/lang/c/src/libavro.a"
    "${_DUCKDB_BUILD_DIR}/tools/cpp/libduckdb_cpp_api.a"
    "${_DUCKDB_DEPS_PREFIX}/lib/libroaring.a"
    "${_DUCKDB_DEPS_PREFIX}/lib/libjansson.a"
  )
ENDIF()
IF(WITH_DUCKDB_HTTPFS)
  LIST(INSERT _DUCKDB_STATIC_LIBS 0
    "${_DUCKDB_BUILD_DIR}/extension/httpfs/libhttpfs_extension.a")
ENDIF()

MESSAGE(STATUS "=== Building DuckDB from submodule (${DUCKDB_SUBMODULE_DIR}) ===")
IF(NOT EXISTS "${DUCKDB_SUBMODULE_DIR}/CMakeLists.txt")
  MESSAGE(FATAL_ERROR "DuckDB submodule is missing: run 'git submodule update --init third_parties/duckdb'")
ENDIF()
IF(NOT EXISTS "${_MAIN_REPO_ROOT}/third_parties/duckdb-quack/CMakeLists.txt")
  MESSAGE(FATAL_ERROR "Quack submodule is missing: run 'git submodule update --init third_parties/duckdb-quack'")
ENDIF()

SET(_QUACK_SUBMODULE_DIR "${_MAIN_REPO_ROOT}/third_parties/duckdb-quack")

# The submodule must be the commit DuckDB itself pins for this release.
FILE(STRINGS "${DUCKDB_SUBMODULE_DIR}/.github/config/extensions/quack.cmake"
     _QUACK_PIN_LINE REGEX "GIT_TAG")
STRING(REGEX MATCH "[0-9a-f]+$" _QUACK_PIN "${_QUACK_PIN_LINE}")
EXECUTE_PROCESS(
  COMMAND git rev-parse HEAD
  WORKING_DIRECTORY "${_QUACK_SUBMODULE_DIR}"
  OUTPUT_VARIABLE _QUACK_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE _rc ERROR_QUIET)
IF(_rc EQUAL 0 AND NOT _QUACK_HEAD STREQUAL _QUACK_PIN)
  MESSAGE(FATAL_ERROR "third_parties/duckdb-quack is at ${_QUACK_HEAD}, "
          "but DuckDB pins Quack ${_QUACK_PIN}")
ENDIF()

SET(_QUACK_PATCHES
  0001-random-token-from-openssl.patch
  0002-disable-quack-serve.patch
  0004-connection-hooks.patch
)
LIST(TRANSFORM _QUACK_PATCHES
     PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/patches/duckdb-quack/")
SET(_QUACK_STAGING_DIR "${CMAKE_CURRENT_BINARY_DIR}/duckdb-quack-staging")
SET(QUACK_SOURCE_DIR   "${CMAKE_CURRENT_BINARY_DIR}/duckdb-quack-src")
FILE(REMOVE_RECURSE "${_QUACK_STAGING_DIR}")
FILE(COPY "${_QUACK_SUBMODULE_DIR}/CMakeLists.txt" "${_QUACK_SUBMODULE_DIR}/src"
     DESTINATION "${_QUACK_STAGING_DIR}")
FIND_PROGRAM(PATCH_EXECUTABLE patch)
IF(NOT PATCH_EXECUTABLE)
  MESSAGE(FATAL_ERROR "'patch' is needed to build Quack")
ENDIF()
FOREACH(_patch IN LISTS _QUACK_PATCHES)
  EXECUTE_PROCESS(
    COMMAND ${PATCH_EXECUTABLE} -p1 --forward --batch --no-backup-if-mismatch
            --fuzz=0 --input "${_patch}"
    WORKING_DIRECTORY "${_QUACK_STAGING_DIR}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  IF(NOT _rc EQUAL 0)
    MESSAGE(FATAL_ERROR "Cannot apply ${_patch} to Quack: ${_out}${_err}")
  ENDIF()
ENDFOREACH()

FILE(GLOB_RECURSE _QUACK_FILES RELATIVE "${_QUACK_STAGING_DIR}"
     "${_QUACK_STAGING_DIR}/*")
FILE(GLOB_RECURSE _QUACK_OLD_FILES RELATIVE "${QUACK_SOURCE_DIR}"
     "${QUACK_SOURCE_DIR}/*")
FOREACH(_f IN LISTS _QUACK_OLD_FILES)
  IF(NOT _f IN_LIST _QUACK_FILES)
    FILE(REMOVE "${QUACK_SOURCE_DIR}/${_f}")
  ENDIF()
ENDFOREACH()
FOREACH(_f IN LISTS _QUACK_FILES)
  CONFIGURE_FILE("${_QUACK_STAGING_DIR}/${_f}" "${QUACK_SOURCE_DIR}/${_f}" COPYONLY)
ENDFOREACH()
FILE(GLOB_RECURSE _QUACK_SUBMODULE_FILES CONFIGURE_DEPENDS
     "${_QUACK_SUBMODULE_DIR}/CMakeLists.txt" "${_QUACK_SUBMODULE_DIR}/src/*")
SET_PROPERTY(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${_QUACK_SUBMODULE_FILES} ${_QUACK_PATCHES})

SET(_DUCKDB_GIT_DESCRIBE "v2.0.0")

SET(LINK_EXTENSION_LIST "")
SET(DESCRIBE_DECLARATIONS "")
SET(DESCRIBE_REGISTRATIONS "")
FOREACH(_ext IN LISTS _DUCKDB_LINKED_EXTENSIONS)
  STRING(APPEND LINK_EXTENSION_LIST " ${_ext}")
  STRING(APPEND DESCRIBE_DECLARATIONS
    "int32_t duckdb_extension_${_ext}_describe(duckdb_extension_descriptor *descriptor);\n")
  STRING(APPEND DESCRIBE_REGISTRATIONS
    "\tif (duckdb_register_static_extension(duckdb_extension_${_ext}_describe) != 0) {\n\t\tresult = 1;\n\t}\n")
ENDFOREACH()
STRING(STRIP "${LINK_EXTENSION_LIST}" LINK_EXTENSION_LIST)
SET(DUCKDB_STATIC_EXTENSION_LOADER
    "${CMAKE_CURRENT_BINARY_DIR}/duckdb_static_extension_loader.c")
CONFIGURE_FILE(
  "${DUCKDB_SUBMODULE_DIR}/extension/loader/static_extension_loader.c.in"
  "${DUCKDB_STATIC_EXTENSION_LOADER}" @ONLY)

SET(_DUCKDB_EXTRA_CXX_FLAGS "")
CHECK_CXX_COMPILER_FLAG("-Wsfinae-incomplete" have_CXX__Wsfinae_incomplete)
IF(have_CXX__Wsfinae_incomplete)
  STRING(APPEND _DUCKDB_EXTRA_CXX_FLAGS " -Wno-sfinae-incomplete")
ENDIF()

IF(WITH_ASAN)
  SET(ADDRESS_SANITIZER TRUE)
ELSE()
  SET(ADDRESS_SANITIZER FALSE)
ENDIF()

IF(WITH_UBSAN)
  SET(UB_SANITIZER TRUE)
ELSE()
  SET(UB_SANITIZER FALSE)
ENDIF()

IF(NOT _DUCKDB_PREBUILT)
  ExternalProject_Add(duckdb_build
    PREFIX          "${_DUCKDB_BUILD_DIR}"
    SOURCE_DIR      "${DUCKDB_SUBMODULE_DIR}"
    BINARY_DIR      "${_DUCKDB_BUILD_DIR}"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=${_DUCKDB_BUILD_TYPE}
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
      "-DCMAKE_PREFIX_PATH=${_DUCKDB_DEPS_PREFIX}"
      -DBUILD_SHELL=OFF
      -DBUILD_UNITTESTS=OFF
      -DENABLE_UNITTEST_CPP_TESTS=OFF
      -DBUILD_PYTHON=OFF
      -DBUILD_BENCHMARKS=OFF
      -DBUILD_TPCE=OFF
      -DEXTENSION_STATIC_BUILD=1
      "-DDUCKDB_EXTENSION_CONFIGS=${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_extensions.cmake"
      "-DMARIADB_QUACK_SOURCE_DIR=${QUACK_SOURCE_DIR}"
      "-DOPENSSL_INCLUDE_DIR=${OPENSSL_INCLUDE_DIR}"
      "-DOPENSSL_SSL_LIBRARY=${OPENSSL_SSL_LIBRARY}"
      "-DOPENSSL_CRYPTO_LIBRARY=${OPENSSL_CRYPTO_LIBRARY}"
      "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS} ${_DUCKDB_EXTRA_CXX_FLAGS}"
      "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS} -std=gnu17"
      "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
      "-DCMAKE_MODULE_LINKER_FLAGS=${CMAKE_MODULE_LINKER_FLAGS}"
      "-DCMAKE_C_LINKER_LAUNCHER=${CMAKE_C_LINKER_LAUNCHER}"
      "-DCMAKE_CXX_LINKER_LAUNCHER=${CMAKE_CXX_LINKER_LAUNCHER}"
      -DENABLE_SANITIZER=${ADDRESS_SANITIZER}
      -DENABLE_UBSAN=${UB_SANITIZER}
      -DOVERRIDE_GIT_DESCRIBE=${_DUCKDB_GIT_DESCRIBE}
      -DWITH_DUCKDB_HTTPFS=${WITH_DUCKDB_HTTPFS}
      -DWITH_DUCKDB_ICEBERG=${WITH_DUCKDB_ICEBERG}
    INSTALL_COMMAND  ""
    BUILD_BYPRODUCTS ${_DUCKDB_STATIC_LIBS}
    USES_TERMINAL_BUILD ON
  )

  FILE(GLOB_RECURSE _QUACK_STAGED_FILES "${QUACK_SOURCE_DIR}/*")
  ExternalProject_Add_StepDependencies(duckdb_build build ${_QUACK_STAGED_FILES})
ENDIF()

ADD_LIBRARY(libduckdb INTERFACE)
TARGET_LINK_LIBRARIES(libduckdb INTERFACE -Wl,--start-group ${_DUCKDB_STATIC_LIBS} -Wl,--end-group)
IF(WITH_DUCKDB_HTTPFS)
  TARGET_LINK_LIBRARIES(libduckdb INTERFACE CURL::libcurl)
ENDIF()
IF(WITH_DUCKDB_ICEBERG)
  FIND_PACKAGE(ZLIB QUIET)
  IF(ZLIB_FOUND)
    TARGET_LINK_LIBRARIES(libduckdb INTERFACE ZLIB::ZLIB)
  ENDIF()
  FIND_PACKAGE(LibLZMA QUIET)
  IF(LibLZMA_FOUND)
    TARGET_LINK_LIBRARIES(libduckdb INTERFACE LibLZMA::LibLZMA)
  ENDIF()
  FIND_PACKAGE(Snappy CONFIG QUIET)
  IF(Snappy_FOUND)
    TARGET_LINK_LIBRARIES(libduckdb INTERFACE Snappy::snappy)
  ENDIF()
  FIND_PACKAGE(zstd CONFIG QUIET)
  IF(zstd_FOUND)
    IF(TARGET zstd::libzstd_static)
      TARGET_LINK_LIBRARIES(libduckdb INTERFACE zstd::libzstd_static)
    ELSEIF(TARGET zstd::libzstd_shared)
      TARGET_LINK_LIBRARIES(libduckdb INTERFACE zstd::libzstd_shared)
    ENDIF()
  ENDIF()
  FIND_PACKAGE(BZip2 QUIET)
  IF(BZIP2_FOUND)
    TARGET_LINK_LIBRARIES(libduckdb INTERFACE BZip2::BZip2)
  ENDIF()
ENDIF()
# OpenSSL after the archives: libquack_extension.a (and httpfs) use it.
TARGET_LINK_LIBRARIES(libduckdb INTERFACE OpenSSL::SSL OpenSSL::Crypto)
IF(NOT _DUCKDB_PREBUILT)
  ADD_DEPENDENCIES(libduckdb duckdb_build)
ENDIF()

MESSAGE(STATUS "DuckDB include: ${DUCKDB_INCLUDE_DIR}")
MESSAGE(STATUS "DuckDB libs:    ${_DUCKDB_STATIC_LIBS}")

INCLUDE_DIRECTORIES(BEFORE SYSTEM "${DUCKDB_INCLUDE_DIR}")
INCLUDE_DIRECTORIES(BEFORE SYSTEM "${DUCKDB_SUBMODULE_DIR}/third_party/re2")
SET(DUCKDB_LIBRARY libduckdb)
