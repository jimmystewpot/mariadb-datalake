#
# Common compile settings for all DuckDB plugin sub-libraries.
# Usage:  duckdb_setup_target(<target_name>)
#
INCLUDE(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-Wcast-align=strict" DUCKDB_HAS_WCAST_ALIGN_STRICT)
CHECK_CXX_COMPILER_FLAG("-Wsfinae-incomplete" have_CXX__Wsfinae_incomplete)

FUNCTION(duckdb_setup_target target)
  # DuckDB headers require C++17.
  SET_TARGET_PROPERTIES(${target} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
  )
  # libduckdb_bundle.a is built without debug STL wrappers.
  # -U unconditionally undefines the macro no matter how it was inherited
  # (CMAKE_CXX_FLAGS_DEBUG, directory properties, generator expressions, etc.).
  # Mismatched _GLIBCXX_DEBUG changes sizeof(std::vector) and friends -> SIGSEGV.
  TARGET_COMPILE_OPTIONS(${target} PRIVATE
    -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS
  )
  TARGET_COMPILE_OPTIONS(${target} PRIVATE
    -Wall
    -Wextra
    -Wcast-qual
    -Wconversion
    -Wsign-conversion
    -pedantic
    -Wshadow
  )
  IF(DUCKDB_HAS_WCAST_ALIGN_STRICT)
    TARGET_COMPILE_OPTIONS(${target} PRIVATE -Wcast-align=strict)
  ELSE()
    TARGET_COMPILE_OPTIONS(${target} PRIVATE -Wcast-align)
  ENDIF()
  # GCC 16+ warns about DuckDB's CompressionInfo in SFINAE contexts.
  # Silence it — upstream DuckDB issue, not ours to fix.
  IF(have_CXX__Wsfinae_incomplete)
    TARGET_COMPILE_OPTIONS(${target} PRIVATE -Wno-sfinae-incomplete)
  ENDIF()
  IF(DUCKDB_WERROR)
    TARGET_COMPILE_OPTIONS(${target} PRIVATE -Werror)
  ENDIF()
  IF(WITH_ASAN)
    TARGET_COMPILE_OPTIONS(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    TARGET_LINK_OPTIONS(${target} PRIVATE -fsanitize=address)
  ENDIF()
  IF(WITH_UBSAN)
    TARGET_COMPILE_OPTIONS(${target} PRIVATE -fsanitize=undefined,bounds,object-size -fno-sanitize-recover=all -fno-omit-frame-pointer)
    TARGET_LINK_OPTIONS(${target} PRIVATE -fsanitize=undefined,bounds,object-size -fno-sanitize-recover=all)
  ENDIF()
ENDFUNCTION()
