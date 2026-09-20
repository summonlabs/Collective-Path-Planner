# Collective Path Planner - test suites.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0 (see LICENSE).

add_library(cpath_test_support STATIC
  support/exact_solver.cpp
  support/fixtures.cpp
  support/process.cpp
  support/test_framework.cpp)
target_link_libraries(cpath_test_support PUBLIC cpath)
cpath_apply_warnings(cpath_test_support)
target_include_directories(cpath_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")

# Each suite is a separate executable and a separate ctest entry so that a
# failure localises to one proof surface.
set(CPATH_TEST_SUITES
  unit
  property
  adversarial
  algorithm
  reference
  persistence
  protocol
  concurrency
  integration
  multiprocess
  scale)

foreach(suite IN LISTS CPATH_TEST_SUITES)
  add_executable(cpath_tests_${suite} ${suite}_main.cpp)
  target_link_libraries(cpath_tests_${suite} PRIVATE cpath_test_support)
  cpath_apply_warnings(cpath_tests_${suite})
  target_compile_definitions(cpath_tests_${suite} PRIVATE
    CPATH_DAEMON_PATH="$<TARGET_FILE:cpathd>"
    CPATH_CLI_PATH="$<TARGET_FILE:cpath_cli>")
  if(CPATH_BUILD_TOOLS)
    add_dependencies(cpath_tests_${suite} cpathd cpath_cli)
  endif()
  # No TIMEOUT property is set anywhere: a hanging test is a defect to diagnose,
  # not something to mask with a timeout.
  add_test(NAME ${suite} COMMAND cpath_tests_${suite})
  set_tests_properties(${suite} PROPERTIES LABELS "${suite}")
endforeach()

if(NOT CPATH_BUILD_TOOLS)
  message(WARNING
    "CPATH_BUILD_TESTS=ON requires CPATH_BUILD_TOOLS=ON: the multiprocess suite drives the real "
    "cpathd and cpath executables.")
endif()
