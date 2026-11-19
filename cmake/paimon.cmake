#
# Build Apache Paimon C++ (third_parties/paimon-cpp) static libraries from submodule source.
#

SET(_MAIN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
IF(NOT EXISTS "${_MAIN_REPO_ROOT}/third_parties/paimon-cpp/CMakeLists.txt" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../third_parties/paimon-cpp/CMakeLists.txt")
  GET_FILENAME_COMPONENT(_MAIN_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
ENDIF()

SET(PAIMON_SUBMODULE_DIR "${_MAIN_REPO_ROOT}/third_parties/paimon-cpp")
SET(PAIMON_INCLUDE_DIR   "${PAIMON_SUBMODULE_DIR}/include")
IF(NOT "${_MAIN_REPO_ROOT}" STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}" AND
   EXISTS "${_MAIN_REPO_ROOT}/build/paimon-build/release/libpaimon.a")
  SET(_PAIMON_BUILD_DIR "${_MAIN_REPO_ROOT}/build/paimon-build")
  SET(_PAIMON_PREBUILT TRUE)
ELSE()
  SET(_PAIMON_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/paimon-build")
  SET(_PAIMON_PREBUILT FALSE)
ENDIF()
SET(_PAIMON_ARROW_INCLUDE_DIR "${_PAIMON_BUILD_DIR}/arrow_ep-install/include")

INCLUDE(ExternalProject)

IF(NOT EXISTS "${PAIMON_SUBMODULE_DIR}/CMakeLists.txt")
  MESSAGE(FATAL_ERROR "paimon-cpp submodule is missing: run 'git submodule update --init third_parties/paimon-cpp'")
ENDIF()

FILE(MAKE_DIRECTORY "${_PAIMON_ARROW_INCLUDE_DIR}")

# Static factory registration archives (REGISTER_PAIMON_FACTORY) must be linked
# with --whole-archive so static initializers for local filesystem, parquet,
# avro, blob, and file_index are never stripped by the linker.
SET(_PAIMON_WHOLE_ARCHIVE_LIBS
  "${_PAIMON_BUILD_DIR}/release/libpaimon_local_file_system.a"
  "${_PAIMON_BUILD_DIR}/release/libpaimon_parquet_file_format.a"
  "${_PAIMON_BUILD_DIR}/release/libpaimon_avro_file_format.a"
  "${_PAIMON_BUILD_DIR}/release/libpaimon_blob_file_format.a"
  "${_PAIMON_BUILD_DIR}/release/libpaimon_file_index.a"
)

SET(_PAIMON_STATIC_LIBS
  "${_PAIMON_BUILD_DIR}/release/libpaimon_global_index.a"
  "${_PAIMON_BUILD_DIR}/release/libpaimon.a"
  "${_PAIMON_BUILD_DIR}/release/libroaring_bitmap.a"
  "${_PAIMON_BUILD_DIR}/release/libxxhash.a"
  "${_PAIMON_BUILD_DIR}/arrow_ep-install/lib/libparquet.a"
  "${_PAIMON_BUILD_DIR}/arrow_ep-install/lib/libarrow_dataset.a"
  "${_PAIMON_BUILD_DIR}/arrow_ep-install/lib/libarrow_acero.a"
  "${_PAIMON_BUILD_DIR}/arrow_ep-install/lib/libarrow.a"
  "${_PAIMON_BUILD_DIR}/arrow_ep-install/lib/libarrow_bundled_dependencies.a"
  "${_PAIMON_BUILD_DIR}/avro_ep-install/lib/libavrocpp_s.a"
  "${_PAIMON_BUILD_DIR}/tbb_ep-install/lib/libtbb.a"
  "${_PAIMON_BUILD_DIR}/glog_ep-install/lib/libglog.a"
  "${_PAIMON_BUILD_DIR}/fmt_ep-install/lib/libfmt.a"
  "${_PAIMON_BUILD_DIR}/re2_ep-install/lib/libre2.a"
  "${_PAIMON_BUILD_DIR}/zstd_ep-install/lib/libzstd.a"
  "${_PAIMON_BUILD_DIR}/snappy_ep-install/lib/libsnappy.a"
  "${_PAIMON_BUILD_DIR}/lz4_ep-install/lib/liblz4.a"
  "${_PAIMON_BUILD_DIR}/zlib_ep-install/lib/libz.a"
)

IF(NOT _PAIMON_PREBUILT)
  SET(_PAIMON_BUILD_CMD "")
  IF(CMAKE_GENERATOR MATCHES "Makefiles")
    SET(_PAIMON_BUILD_CMD BUILD_COMMAND ${CMAKE_COMMAND} -E env CMAKE_POLICY_VERSION_MINIMUM=3.5 LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu $(MAKE))
  ENDIF()

  ExternalProject_Add(paimon_build
    PREFIX          "${_PAIMON_BUILD_DIR}"
    SOURCE_DIR      "${PAIMON_SUBMODULE_DIR}"
    BINARY_DIR      "${_PAIMON_BUILD_DIR}"
    CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5
      -DPAIMON_DEPENDENCY_SOURCE=BUNDLED
      -DPAIMON_BUILD_STATIC=ON
      -DPAIMON_BUILD_SHARED=OFF
      -DPAIMON_BUILD_TESTS=OFF
      -DPAIMON_BUILD_BENCHMARKS=OFF
      -DPAIMON_ENABLE_AVRO=ON
      -DPAIMON_ENABLE_ORC=OFF
      -DPAIMON_ENABLE_LANCE=OFF
      -DPAIMON_ENABLE_MOSAIC=OFF
      -DPAIMON_ENABLE_VORTEX=OFF
      -DPAIMON_ENABLE_LUCENE=OFF
      -DPAIMON_ENABLE_LUMINA=OFF
      -DPAIMON_ENABLE_JINDO=OFF
      -DPAIMON_ENABLE_OSS=OFF
      -DPAIMON_ENABLE_S3=OFF
      -DPAIMON_ENABLE_REST=OFF
      -DPAIMON_ENABLE_FULL_TEXT=OFF
      "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS} -fPIC -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS -Wno-error"
      "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS} -fPIC -std=gnu17 -Wno-error"
    ${_PAIMON_BUILD_CMD}
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS ${_PAIMON_WHOLE_ARCHIVE_LIBS} ${_PAIMON_STATIC_LIBS}
    USES_TERMINAL_BUILD ON
  )
ENDIF()

ADD_LIBRARY(libpaimon INTERFACE)
TARGET_LINK_LIBRARIES(libpaimon INTERFACE
  -Wl,--whole-archive ${_PAIMON_WHOLE_ARCHIVE_LIBS} -Wl,--no-whole-archive
  -Wl,--start-group ${_PAIMON_STATIC_LIBS} -Wl,--end-group
  pthread dl
)
FIND_LIBRARY(LIBUNWIND_LIBRARY NAMES unwind)
IF(LIBUNWIND_LIBRARY)
  TARGET_LINK_LIBRARIES(libpaimon INTERFACE ${LIBUNWIND_LIBRARY})
ENDIF()
TARGET_INCLUDE_DIRECTORIES(libpaimon SYSTEM INTERFACE
  "${PAIMON_INCLUDE_DIR}"
  "${_PAIMON_ARROW_INCLUDE_DIR}"
)
IF(NOT _PAIMON_PREBUILT)
  ADD_DEPENDENCIES(libpaimon paimon_build)
ENDIF()
