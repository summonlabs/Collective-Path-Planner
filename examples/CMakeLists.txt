# Collective Path Planner - examples.
#
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0 (see LICENSE).

add_executable(cpath_example_plan_ring plan_ring.cpp)
target_link_libraries(cpath_example_plan_ring PRIVATE cpath)
cpath_apply_warnings(cpath_example_plan_ring)

add_executable(cpath_example_explain_denial explain_denial.cpp)
target_link_libraries(cpath_example_explain_denial PRIVATE cpath)
cpath_apply_warnings(cpath_example_explain_denial)

add_executable(cpath_example_store_replay store_replay.cpp)
target_link_libraries(cpath_example_store_replay PRIVATE cpath)
cpath_apply_warnings(cpath_example_store_replay)

if(CPATH_BUILD_TESTS)
  # The examples are executed as part of the standard test run so that a broken
  # example is a failed build, not a broken promise in the README.
  add_test(NAME example_plan_ring COMMAND cpath_example_plan_ring)
  add_test(NAME example_explain_denial COMMAND cpath_example_explain_denial)
  add_test(NAME example_store_replay COMMAND cpath_example_store_replay)
  set_tests_properties(example_plan_ring example_explain_denial example_store_replay
    PROPERTIES LABELS "examples")
endif()
