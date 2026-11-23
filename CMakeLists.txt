cmake_minimum_required(VERSION 3.12)
project(duckdb LANGUAGES C CXX)
find_package(mariadb-plugin QUIET)
INCLUDE("${CMAKE_CURRENT_SOURCE_DIR}/VERSIONS")
INCLUDE(CheckCXXCompilerFlag)
ENABLE_TESTING()

IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  MESSAGE(STATUS "DuckDB: not Linux, skipping")
  return()
ENDIF()

IF(NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
        CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64"))
  MESSAGE(STATUS "DuckDB: not x86_64 or aarch64, skipping")
  return()
ENDIF()

# Catch -m32 / multilib builds (e.g. 32-bit build inside an x86_64 CI
# container): CMAKE_SYSTEM_PROCESSOR still reports x86_64, but the target is
# 32-bit. DuckDB (and its jemalloc extension) is 64-bit only.
IF(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  MESSAGE(STATUS "DuckDB: not a 64-bit build, skipping")
  return()
ENDIF()

# Check C++17 compiler support
CHECK_CXX_COMPILER_FLAG("-std=c++17" have_CXX__std_c__17)
IF(NOT have_CXX__std_c__17)
  MESSAGE(STATUS "DuckDB: C++ compiler does not support -std=c++17, skipping")
  RETURN()
ENDIF()

# libduckdb_bundle.a is built without debug STL wrappers.
# Mismatched _GLIBCXX_DEBUG changes sizeof(std::vector) → SIGSEGV.
SET(CMAKE_CXX_FLAGS_DEBUG
    "${CMAKE_CXX_FLAGS_DEBUG} -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS")

# Plugin sources (handler layer + UDFs whose extern "C" symbols must be
# in the final .so for dlsym() loading).
SET(DUCKDB_PLUGIN_SOURCES
  ha_duckdb.cc
  ha_duckdb_pushdown.cc
  ha_duckdb_quack.cc
  duckdb_udf.cc
  duckdb_udf_wrappers.cc
  bridge/bridge_test_udf.cc
)

IF(mariadb-plugin_FOUND AND NOT STANDALONE_TESTS)
  MARIADB_ADD_PLUGIN(duckdb ${DUCKDB_PLUGIN_SOURCES} STORAGE_ENGINE MODULE_ONLY
                     AUTHOR "drrtuy,lfedorov"
                     VERSION "${DUCKDB_PLUGIN_VERSION}"
                     DESCRIPTION "DuckDB storage engine"
                     CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/duckdb.cnf)

  SET(CPACK_RPM_duckdb-engine_PACKAGE_DESCRIPTION "The MariaDB DuckDB storage
engine embeds DuckDB as a pluggable storage engine, enabling fast analytical
(OLAP) query processing directly inside MariaDB.")
ENDIF()

IF(TARGET duckdb)

  # Quack (MDEV-41383) serves HTTPS through OpenSSL. Linked dynamically: on
  # distribution builds this is the same libssl/libcrypto mariadbd loads.
  FIND_PACKAGE(OpenSSL)
  IF(NOT OPENSSL_FOUND)
    MESSAGE(FATAL_ERROR "DuckDB: OpenSSL development files are required "
            "(libssl-dev / openssl-devel)")
  ENDIF()

  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb.cmake)
  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/paimon.cmake)
  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

  # DuckDB-side Quack and Lakehouse modules; added first so they get no MariaDB headers.
  ADD_SUBDIRECTORY(quack_guard)
  ADD_SUBDIRECTORY(lakehouse)

  # Provide MariaDB server include paths to sub-libraries.
  # MARIADB_ADD_PLUGIN does this only for the plugin target itself.
  LINK_LIBRARIES(mariadb_private mysqlservices)

  # Build sub-libraries.
  ADD_SUBDIRECTORY(common)
  ADD_SUBDIRECTORY(convertor)
  ADD_SUBDIRECTORY(runtime)
  ADD_SUBDIRECTORY(bridge)

  # atomic: GCC lowers the 16-byte atomic ops of the AUTO_INCREMENT block
  # cache (Duckdb_share::autoinc_range) to libatomic calls.
  target_link_libraries(duckdb duckdb_runtime duckdb_convertor duckdb_common
    duckdb_lakehouse quack_bridge quack_guard ${DUCKDB_LIBRARY} libpaimon atomic
  )
  TARGET_SOURCES(duckdb PRIVATE ${DUCKDB_STATIC_EXTENSION_LOADER})

  duckdb_setup_target(duckdb)
  TARGET_INCLUDE_DIRECTORIES(duckdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/convertor
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/lakehouse/include
  )

  IF(DUCKDB_UNIT_TESTS)
    IF(TARGET quack_guard_test)
      ADD_TEST(NAME quack_guard_test COMMAND quack_guard_test)
    ENDIF()
    IF(TARGET mariadb_dialect_test)
      ADD_TEST(NAME mariadb_dialect_test COMMAND mariadb_dialect_test)
    ENDIF()
    IF(TARGET fiber_context_test)
      ADD_TEST(NAME fiber_context_test COMMAND fiber_context_test)
    ENDIF()
  ENDIF()
ELSEIF(STANDALONE_TESTS OR NOT mariadb-plugin_FOUND)
  MESSAGE(STATUS "DuckDB: mariadb-plugin SDK not found; building Layer A standalone targets")
  SET(DUCKDB_UNIT_TESTS ON)
  FIND_PACKAGE(OpenSSL REQUIRED)

  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb.cmake)
  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/paimon.cmake)
  INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

  ADD_SUBDIRECTORY(quack_guard)
  IF(TARGET quack_guard_test)
    ADD_TEST(NAME quack_guard_test COMMAND quack_guard_test)
  ENDIF()

  ADD_SUBDIRECTORY(lakehouse)

  ADD_EXECUTABLE(mariadb_dialect_test
    runtime/mariadb_dialect.cc
    runtime/mariadb_dialect_expression.cc
    runtime/mariadb_dialect_select.cc
    common/mariadb_lexical_guard.cc
    runtime/mariadb_dialect_test.cc
    runtime/mariadb_dialect_expression_test.cc
    runtime/mariadb_dialect_select_test.cc
    ${DUCKDB_STATIC_EXTENSION_LOADER}
  )
  duckdb_setup_target(mariadb_dialect_test)
  SET_TARGET_PROPERTIES(mariadb_dialect_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")
  TARGET_INCLUDE_DIRECTORIES(mariadb_dialect_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${DUCKDB_SUBMODULE_DIR}/third_party/utf8proc/include)
  TARGET_LINK_LIBRARIES(mariadb_dialect_test PRIVATE ${DUCKDB_LIBRARY}
    pthread dl
    -Wl,--wrap=_ZN6duckdb21PEGTransformerFactory26TransformTopLevelStatementERNS_13TokenIteratorERNS_13ParserOptionsERKNS_15CompiledGrammarE)
  ADD_TEST(NAME mariadb_dialect_test COMMAND mariadb_dialect_test)

  ADD_EXECUTABLE(fiber_context_test
    runtime/fiber_context_test.cc
    runtime/fiber_context.cpp
  )
  TARGET_INCLUDE_DIRECTORIES(fiber_context_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  )
  duckdb_setup_target(fiber_context_test)
  CHECK_CXX_COMPILER_FLAG("-Wno-frame-larger-than" DUCKDB_HAS_WNO_FRAME_LARGER_THAN)
  IF(DUCKDB_HAS_WNO_FRAME_LARGER_THAN)
    TARGET_COMPILE_OPTIONS(fiber_context_test PRIVATE -Wno-frame-larger-than)
  ENDIF()
  ADD_TEST(NAME fiber_context_test COMMAND fiber_context_test)
ENDIF()

IF(mariadb-plugin_FOUND)
  include(CPack)
ENDIF()
