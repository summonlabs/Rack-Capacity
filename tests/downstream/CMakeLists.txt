# Rack Capacity - independent downstream consumer.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project is deliberately not part of the Rack Capacity build. It exists to
# prove that the installed package is usable by a consumer that has never seen
# this source tree: it is configured against an install prefix, finds the
# package with find_package, builds from sources that live elsewhere and runs a
# real minimal lifecycle.
#
# Configure it with:
#   cmake -S tests/downstream -B <build-dir> -G Ninja \
#         -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_PREFIX_PATH=<install-prefix>

cmake_minimum_required(VERSION 3.20)

project(RackCapacityDownstreamConsumer
        VERSION 1.0.0
        DESCRIPTION "Independent consumer of the installed Rack Capacity package"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(RackCapacity 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE RackCapacity::rack_capacity)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
add_test(NAME downstream_consumer_runs COMMAND downstream_consumer)

# The package must state the versions it was built with, and the consumer must
# be able to read them without compiling anything from this repository.
message(STATUS "Rack Capacity package version      : ${RackCapacity_VERSION}")
message(STATUS "Rack Capacity state format version : ${RackCapacity_STATE_FORMAT_VERSION}")
message(STATUS "Rack Capacity snapshot layout      : ${RackCapacity_SNAPSHOT_LAYOUT_VERSION}")
message(STATUS "Rack Capacity slots per rack unit  : ${RackCapacity_MOUNT_SLOTS_PER_RACK_UNIT}")
