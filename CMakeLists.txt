# Rack Capacity - build, test, install and packaging rules.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Everything here is target based: no directory-global flags are set, so the
# library, the tools, the benchmarks and the tests each carry exactly the
# options they need and nothing this project uses for itself can leak into the
# installed package.

cmake_minimum_required(VERSION 3.20)

project(RackCapacity
        VERSION 1.0.0
        DESCRIPTION "Per-rack capacity accounting runtime for the Data Center Control Plane"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------

option(BUILD_SHARED_LIBS "Build rack_capacity as a shared library instead of a static one" OFF)

option(RC_BUILD_TESTS "Build the Rack Capacity test suite" ON)
option(RC_BUILD_TOOLS "Build the Rack Capacity command line tools" ON)
option(RC_BUILD_EXAMPLES "Build the Rack Capacity example programs" ON)
option(RC_BUILD_BENCHMARKS "Build the Rack Capacity benchmarks" ON)
option(RC_WARNINGS_AS_ERRORS "Treat every compiler warning as an error" ON)
option(RC_SANITIZERS "Build with AddressSanitizer (and UndefinedBehaviorSanitizer on GCC/Clang)" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

set(RC_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/RackCapacity"
    CACHE STRING "Install destination of the Rack Capacity CMake package files")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Applies the project build policy to one target. Every entry is PRIVATE, so
# none of it becomes part of the installed package.
function(rc_configure_target target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
    if(RC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Release>:/O2>)
    target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:/Od>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(RC_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()

  if(RC_SANITIZERS)
    if(MSVC)
      target_compile_options(${target} PRIVATE /fsanitize=address)
    else()
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# Adds one executable and applies the project build policy to it.
function(rc_add_executable target)
  add_executable(${target} ${ARGN})
  rc_configure_target(${target})
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------

set(RC_PUBLIC_HEADERS
  include/rack_capacity/bound.hpp
  include/rack_capacity/capacity.hpp
  include/rack_capacity/catalog.hpp
  include/rack_capacity/coordinates.hpp
  include/rack_capacity/diff.hpp
  include/rack_capacity/digest.hpp
  include/rack_capacity/errors.hpp
  include/rack_capacity/evidence.hpp
  include/rack_capacity/export.hpp
  include/rack_capacity/fit.hpp
  include/rack_capacity/ids.hpp
  include/rack_capacity/limits.hpp
  include/rack_capacity/measures.hpp
  include/rack_capacity/persistence.hpp
  include/rack_capacity/rack_capacity.hpp
  include/rack_capacity/requests.hpp
  include/rack_capacity/result.hpp
  include/rack_capacity/text.hpp
  include/rack_capacity/version.hpp)

set(RC_SOURCES
  src/capacity.cpp
  src/catalog.cpp
  src/coordinates.cpp
  src/diff.cpp
  src/digest.cpp
  src/errors.cpp
  src/evidence.cpp
  src/fit.cpp
  src/ids.cpp
  src/measures.cpp
  src/persistence.cpp
  src/report.cpp
  src/serialization.cpp
  src/store_files.cpp
  src/text.cpp
  src/version.cpp)

add_library(rack_capacity ${RC_SOURCES} ${RC_PUBLIC_HEADERS})
add_library(RackCapacity::rack_capacity ALIAS rack_capacity)

target_include_directories(rack_capacity
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

set_target_properties(rack_capacity PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON)

rc_configure_target(rack_capacity)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(rack_capacity
    PUBLIC RACK_CAPACITY_SHARED
    PRIVATE RACK_CAPACITY_BUILDING)
endif()

# ---------------------------------------------------------------------------
# Tools, examples, tests and benchmarks
# ---------------------------------------------------------------------------

if(RC_BUILD_TOOLS)
  rc_add_executable(rackcap tools/rackcap.cpp)
  target_link_libraries(rackcap PRIVATE RackCapacity::rack_capacity)

  if(RC_BUILD_TESTS)
    # Process-level proof harness. It is a real program that a test starts as a
    # separate operating-system process to compete for writer authority and to
    # die abruptly in the middle of the publication protocol. It is not
    # installed.
    rc_add_executable(rc_child tools/rc_child.cpp)
    target_link_libraries(rc_child PRIVATE RackCapacity::rack_capacity)
  endif()
endif()

if(RC_BUILD_EXAMPLES)
  foreach(example IN ITEMS
      example_capacity_basics
      example_fit_evaluation
      example_durable_recovery)
    rc_add_executable(${example} examples/${example}.cpp)
    target_link_libraries(${example} PRIVATE RackCapacity::rack_capacity)
  endforeach()
endif()

if(RC_BUILD_BENCHMARKS)
  rc_add_executable(bench_rack_capacity benchmarks/bench_rack_capacity.cpp)
  target_link_libraries(bench_rack_capacity PRIVATE RackCapacity::rack_capacity)
endif()

if(RC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install and package
# ---------------------------------------------------------------------------

install(TARGETS rack_capacity
        EXPORT RackCapacityTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/rack_capacity
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

if(RC_BUILD_TOOLS)
  install(TARGETS rackcap RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(EXPORT RackCapacityTargets
        FILE RackCapacityTargets.cmake
        NAMESPACE RackCapacity::
        DESTINATION ${RC_INSTALL_CMAKEDIR})

configure_package_config_file(
  cmake/RackCapacityConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/RackCapacityConfig.cmake"
  INSTALL_DESTINATION ${RC_INSTALL_CMAKEDIR})

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/RackCapacityConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/RackCapacityConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/RackCapacityConfigVersion.cmake"
        DESTINATION ${RC_INSTALL_CMAKEDIR})

install(FILES README.md LICENSE NOTICE
        DESTINATION ${CMAKE_INSTALL_DOCDIR})

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------

message(STATUS "Rack Capacity ${PROJECT_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  shared library      : ${BUILD_SHARED_LIBS}")
message(STATUS "  tests               : ${RC_BUILD_TESTS}")
message(STATUS "  tools               : ${RC_BUILD_TOOLS}")
message(STATUS "  examples            : ${RC_BUILD_EXAMPLES}")
message(STATUS "  benchmarks          : ${RC_BUILD_BENCHMARKS}")
message(STATUS "  warnings as errors  : ${RC_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${RC_SANITIZERS}")
