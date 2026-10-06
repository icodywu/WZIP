# WZIP and WLZ4: libraries (static and shared), the wzip and wlz4 tools, tests, install and pkg-config.
#   cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build-cmake && ctest --test-dir build-cmake
#   cmake --install build-cmake --prefix /usr/local
cmake_minimum_required(VERSION 3.13)
project(wzip VERSION 1.0.0 DESCRIPTION "WZIP and WLZ4 compression" LANGUAGES C)

option(WZIP_BUILD_SHARED   "Build the shared library"         ON)
option(WZIP_BUILD_PROGRAMS "Build the wzip and wlz4 tools"    ON)
option(WZIP_BUILD_TESTS    "Build the tests"                  ON)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall)
endif()
include(GNUInstallDirs)

set(WZIP_SOURCES
  src/WZIP_L.c src/WZIP_M.c src/WZIP_S.c src/WZIP_wrapper.c
  src/Huffman_Compress.c src/Huffman_Decompress.c src/WLZ4.c src/wzframe.c)
set(WZIP_HEADERS src/WZIP.h src/WLZ4.h src/wzframe.h)
find_library(MATH_LIBRARY m)

add_library(wzip_static STATIC ${WZIP_SOURCES})
set_target_properties(wzip_static PROPERTIES OUTPUT_NAME wzip POSITION_INDEPENDENT_CODE ON)
target_include_directories(wzip_static PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
                                              $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(MATH_LIBRARY)
  target_link_libraries(wzip_static PUBLIC ${MATH_LIBRARY})
endif()
set(WZIP_TARGETS wzip_static)

if(WZIP_BUILD_SHARED)
  add_library(wzip_shared SHARED ${WZIP_SOURCES})
  set_target_properties(wzip_shared PROPERTIES OUTPUT_NAME wzip VERSION ${PROJECT_VERSION}
                        SOVERSION ${PROJECT_VERSION_MAJOR} WINDOWS_EXPORT_ALL_SYMBOLS ON)
  if(WIN32)
    set_target_properties(wzip_shared PROPERTIES ARCHIVE_OUTPUT_NAME wzip_dll)
  endif()
  target_include_directories(wzip_shared PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
                                                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  if(MATH_LIBRARY)
    target_link_libraries(wzip_shared PRIVATE ${MATH_LIBRARY})
  endif()
  list(APPEND WZIP_TARGETS wzip_shared)
endif()

if(WZIP_BUILD_PROGRAMS)
  add_executable(wzip programs/wzip.c)
  target_link_libraries(wzip PRIVATE wzip_static)
  add_executable(wlz4 programs/wzip.c)            # the same program; its name selects WLZ4
  target_link_libraries(wlz4 PRIVATE wzip_static)
  list(APPEND WZIP_TARGETS wzip wlz4)
endif()

if(WZIP_BUILD_TESTS)
  enable_testing()
  find_package(Threads)
  add_executable(roundtrip tests/roundtrip.c)
  target_link_libraries(roundtrip PRIVATE wzip_static)
  add_test(NAME roundtrip COMMAND roundtrip)
  if(Threads_FOUND)
    add_executable(threads tests/threads.c)
    target_link_libraries(threads PRIVATE wzip_static Threads::Threads)
    add_test(NAME threads COMMAND threads)
  endif()
  find_program(SH sh)
  if(SH AND WZIP_BUILD_PROGRAMS)
    add_test(NAME cli COMMAND ${SH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.sh $<TARGET_FILE_DIR:wzip>)
  endif()
endif()

install(TARGETS ${WZIP_TARGETS}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${WZIP_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_file(libwzip.pc.in libwzip.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/libwzip.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
