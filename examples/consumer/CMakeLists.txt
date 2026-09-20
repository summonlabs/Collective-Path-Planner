cmake_minimum_required(VERSION 3.20)

# Independent downstream consumer of the installed Collective Path Planner
# package. This project deliberately does NOT add the planner as a subdirectory:
# it consumes exactly what cmake --install produced, through find_package.
project(CollectivePathPlannerConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent downstream consumer of the installed Collective Path Planner package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CollectivePathPlanner 1.0 CONFIG REQUIRED)

add_executable(cpath_consumer main.cpp)
target_link_libraries(cpath_consumer PRIVATE CollectivePathPlanner::cpath)

if(MSVC)
  target_compile_options(cpath_consumer PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /utf-8 /EHsc)
else()
  target_compile_options(cpath_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()
add_test(NAME consumer_produces_and_revalidates_a_plan COMMAND cpath_consumer)

message(STATUS "Collective Path Planner consumer configured against "
  "${CollectivePathPlanner_VERSION} at ${CollectivePathPlanner_DIR}")
