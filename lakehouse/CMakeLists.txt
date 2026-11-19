INCLUDE(${PROJECT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

ADD_LIBRARY(duckdb_lakehouse STATIC
  src/lakehouse_connection.cc
  src/lakehouse_catalog.cc
  src/paimon_arrow_stream.cc
  src/paimon_table_function.cc
)
duckdb_setup_target(duckdb_lakehouse)

TARGET_INCLUDE_DIRECTORIES(duckdb_lakehouse
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
TARGET_INCLUDE_DIRECTORIES(duckdb_lakehouse SYSTEM PUBLIC
  ${DUCKDB_SUBMODULE_DIR}/third_party/httplib
  ${DUCKDB_SUBMODULE_DIR}/third_party/mbedtls/include
  ${DUCKDB_SUBMODULE_DIR}/third_party/fmt/include
  ${OPENSSL_INCLUDE_DIR}
)
TARGET_LINK_LIBRARIES(duckdb_lakehouse PUBLIC ${DUCKDB_LIBRARY} libpaimon)

IF(DUCKDB_UNIT_TESTS)
  FIND_PACKAGE(GTest QUIET)
  IF(NOT GTest_FOUND)
    INCLUDE(FetchContent)
    SET(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    SET(BUILD_GMOCK ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
    IF(TARGET gtest)
      duckdb_setup_target(gtest)
    ENDIF()
    IF(TARGET gtest_main)
      duckdb_setup_target(gtest_main)
    ENDIF()
  ENDIF()

  MACRO(add_lakehouse_test _target_name)
    ADD_EXECUTABLE(${_target_name}
      ${ARGN}
      ${DUCKDB_STATIC_EXTENSION_LOADER}
    )
    duckdb_setup_target(${_target_name})
    TARGET_INCLUDE_DIRECTORIES(${_target_name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/include
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${PROJECT_SOURCE_DIR}/runtime
      ${PROJECT_SOURCE_DIR}/common
      ${DUCKDB_SUBMODULE_DIR}/third_party/utf8proc/include
    )
    TARGET_INCLUDE_DIRECTORIES(${_target_name} SYSTEM PRIVATE
      ${DUCKDB_SUBMODULE_DIR}/third_party/httplib
      ${DUCKDB_SUBMODULE_DIR}/third_party/mbedtls/include
      ${DUCKDB_SUBMODULE_DIR}/third_party/fmt/include
      ${OPENSSL_INCLUDE_DIR}
    )
    TARGET_COMPILE_DEFINITIONS(${_target_name} PRIVATE
      PAIMON_GOLDEN_DATA_DIR="${PAIMON_SUBMODULE_DIR}/test/test_data"
      LAKEHOUSE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures"
    )
    TARGET_LINK_LIBRARIES(${_target_name} PRIVATE
      duckdb_lakehouse
      ${DUCKDB_LIBRARY}
      libpaimon
      GTest::gtest_main
      GTest::gtest
      atomic
      pthread
      dl
    )
    ADD_TEST(NAME ${_target_name} COMMAND ${_target_name})
  ENDMACRO()

  add_lakehouse_test(smoke_test test/smoke_test.cc)
  add_lakehouse_test(connection_test test/connection_test.cc)
  add_lakehouse_test(paimon_scan_test test/paimon_scan_test.cc)
  add_lakehouse_test(iceberg_catalog_test test/iceberg_catalog_test.cc)
  add_lakehouse_test(lakehouse_e2e_test
    test/lakehouse_e2e_test.cc
    ${PROJECT_SOURCE_DIR}/runtime/mariadb_dialect.cc
    ${PROJECT_SOURCE_DIR}/runtime/mariadb_dialect_expression.cc
    ${PROJECT_SOURCE_DIR}/runtime/mariadb_dialect_select.cc
    ${PROJECT_SOURCE_DIR}/common/mariadb_lexical_guard.cc
  )
ENDIF()
