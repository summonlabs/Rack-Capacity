# Rack Capacity - test suite.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# Each suite is one executable registered as one CTest test. No test declares a
# timeout, uses a watchdog, or treats a killed process as a pass: a test that
# does not terminate is a defect to diagnose.

set(RC_TEST_SUPPORT test_framework.cpp test_support.cpp)

function(rc_add_test target)
  add_executable(${target} ${ARGN} ${RC_TEST_SUPPORT})
  rc_configure_target(${target})
  target_link_libraries(${target} PRIVATE RackCapacity::rack_capacity)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

rc_add_test(test_coordinates test_coordinates.cpp)
rc_add_test(test_measures test_measures.cpp)
rc_add_test(test_evidence test_evidence.cpp)
rc_add_test(test_capacity test_capacity.cpp)
rc_add_test(test_fit test_fit.cpp)
rc_add_test(test_catalog test_catalog.cpp)
rc_add_test(test_persistence test_persistence.cpp)
rc_add_test(test_property test_property.cpp)
rc_add_test(test_adversarial test_adversarial.cpp)
rc_add_test(test_concurrency test_concurrency.cpp)
rc_add_test(test_multiprocess test_multiprocess.cpp)

# The multiprocess suite starts the harness as a real operating-system process.
# The dependency is explicit so a partial build cannot silently skip the proof,
# and the path is passed at compile time so the suite never guesses it.
if(TARGET rc_child)
  target_compile_definitions(test_multiprocess
    PRIVATE RC_CHILD_EXECUTABLE="$<TARGET_FILE:rc_child>")
  add_dependencies(test_multiprocess rc_child)
endif()

rc_add_test(test_cli test_cli.cpp)
if(TARGET rackcap)
  target_compile_definitions(test_cli
    PRIVATE RC_CAPACITY_CLI_EXECUTABLE="$<TARGET_FILE:rackcap>")
  add_dependencies(test_cli rackcap)
endif()