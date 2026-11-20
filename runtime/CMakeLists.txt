INCLUDE(${PROJECT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

SET(DUCKDB_RUNTIME_SOURCES
  duckdb_config.cc
  duckdb_manager.cc
  duckdb_query.cc
  mariadb_dialect.cc
  mariadb_dialect_expression.cc
  mariadb_dialect_select.cc
  duckdb_context.cc
  delta_appender.cc
  cross_engine_scan.cc
  duckdb_mysql_compat.cc
  duckdb_mariadb_tz.cc
  fiber_context.cpp
  fiber_scan.cc
)

ADD_LIBRARY(duckdb_runtime STATIC ${DUCKDB_RUNTIME_SOURCES})
duckdb_setup_target(duckdb_runtime)

TARGET_INCLUDE_DIRECTORIES(duckdb_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
TARGET_INCLUDE_DIRECTORIES(duckdb_runtime PRIVATE
  ${DUCKDB_SUBMODULE_DIR}/third_party/utf8proc/include
  ${DUCKDB_SUBMODULE_DIR}/extension/icu/datetime/include)
TARGET_LINK_LIBRARIES(duckdb_runtime PUBLIC duckdb_convertor duckdb_common)
TARGET_LINK_LIBRARIES(duckdb_runtime PUBLIC quack_guard duckdb_lakehouse)

IF(DUCKDB_UNIT_TESTS)
  ADD_EXECUTABLE(mariadb_dialect_test
    mariadb_dialect_test.cc mariadb_dialect_expression_test.cc
    mariadb_dialect_select_test.cc ${DUCKDB_STATIC_EXTENSION_LOADER})
  duckdb_setup_target(mariadb_dialect_test)
  TARGET_INCLUDE_DIRECTORIES(mariadb_dialect_test PRIVATE
    ${DUCKDB_SUBMODULE_DIR}/third_party/utf8proc/include)
  TARGET_LINK_LIBRARIES(mariadb_dialect_test PRIVATE duckdb_runtime ${DUCKDB_LIBRARY}
    pthread dl
    -Wl,--wrap=_ZN6duckdb21PEGTransformerFactory26TransformTopLevelStatementERNS_13TokenIteratorERNS_13ParserOptionsERKNS_15CompiledGrammarE)

  ADD_EXECUTABLE(fiber_context_test fiber_context_test.cc fiber_context.cpp)
  TARGET_INCLUDE_DIRECTORIES(fiber_context_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  duckdb_setup_target(fiber_context_test)
  INCLUDE(CheckCXXCompilerFlag)
  CHECK_CXX_COMPILER_FLAG("-Wno-frame-larger-than" DUCKDB_HAS_WNO_FRAME_LARGER_THAN)
  IF(DUCKDB_HAS_WNO_FRAME_LARGER_THAN)
    TARGET_COMPILE_OPTIONS(fiber_context_test PRIVATE -Wno-frame-larger-than)
  ENDIF()
ENDIF()
