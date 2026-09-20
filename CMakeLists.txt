cmake_minimum_required(VERSION 3.20)

project(CollectivePathPlanner
  VERSION 1.0.1
  DESCRIPTION "Vendor-neutral C++20 planner for mapping collective communication structures onto generation-bound physical network fabrics"
  HOMEPAGE_URL "https://github.com/summonlabs/Collective-Path-Planner"
  LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
  set(CPATH_IS_TOP_LEVEL ON)
else()
  set(CPATH_IS_TOP_LEVEL OFF)
endif()

option(CPATH_BUILD_TESTS "Build the Collective Path Planner test suites" ${CPATH_IS_TOP_LEVEL})
option(CPATH_BUILD_TOOLS "Build the cpath CLI and cpathd coordinator" ${CPATH_IS_TOP_LEVEL})
option(CPATH_BUILD_EXAMPLES "Build the example programs" ${CPATH_IS_TOP_LEVEL})
option(CPATH_WARNINGS_AS_ERRORS "Treat first-party compiler warnings as errors" ON)
option(CPATH_ENABLE_SANITIZERS "Build with AddressSanitizer/UndefinedBehaviorSanitizer where supported" OFF)
option(CPATH_STRICT_FLOATING_POINT "Reject narrowing and conversion warnings in first-party code" OFF)

if(CPATH_BUILD_TESTS)
  enable_testing()
  include(CTest)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Compiler identification for the generated version header
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "MSVC")
  set(CPATH_COMPILER_ID "MSVC")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  set(CPATH_COMPILER_ID "Clang")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(CPATH_COMPILER_ID "GCC")
else()
  set(CPATH_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
endif()
set(CPATH_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")

set(CPATH_SANITIZERS_ENABLED "false")
if(CPATH_ENABLE_SANITIZERS)
  set(CPATH_SANITIZERS_ENABLED "true")
endif()

set(CPATH_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/cpath/version.hpp.in"
  "${CPATH_GENERATED_DIR}/cpath/version.hpp"
  @ONLY)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
# Warning options are applied with a function rather than an INTERFACE library
# so that they never end up in the installed export set: a consumer must not
# inherit this project's warning policy.
set(CPATH_WARNING_OPTIONS "")
set(CPATH_WARNING_AS_ERROR_OPTION "")

if(MSVC)
  list(APPEND CPATH_WARNING_OPTIONS
    /W4
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /utf-8
    /EHsc
    /MP)
  set(CPATH_WARNING_AS_ERROR_OPTION /WX)
else()
  list(APPEND CPATH_WARNING_OPTIONS
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough)
  set(CPATH_WARNING_AS_ERROR_OPTION -Werror)
endif()

function(cpath_apply_warnings target)
  target_compile_options(${target} PRIVATE ${CPATH_WARNING_OPTIONS})
  if(CPATH_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE ${CPATH_WARNING_AS_ERROR_OPTION})
  endif()
endfunction()

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
file(GLOB CPATH_PUBLIC_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/cpath/*.hpp")

set(CPATH_SOURCES
  src/buffer.cpp
  src/collective.cpp
  src/digest.cpp
  src/dsl.cpp
  src/explain.cpp
  src/fabric.cpp
  src/ids.cpp
  src/net.cpp
  src/persistence.cpp
  src/plan.cpp
  src/planner.cpp
  src/policy.cpp
  src/request.cpp
  src/server.cpp
  src/status.cpp
  src/version.cpp
  src/wire.cpp)

add_library(cpath ${CPATH_SOURCES})
add_library(CollectivePathPlanner::cpath ALIAS cpath)

target_include_directories(cpath
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${CPATH_GENERATED_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src")

cpath_apply_warnings(cpath)
target_compile_features(cpath PUBLIC cxx_std_20)

set_target_properties(cpath PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  OUTPUT_NAME "cpath"
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  DEFINE_SYMBOL "CPATH_BUILDING_LIBRARY")

if(WIN32)
  target_link_libraries(cpath PRIVATE ws2_32)
endif()

if(CPATH_ENABLE_SANITIZERS)
  if(MSVC)
    target_compile_options(cpath PUBLIC /fsanitize=address)
    target_link_options(cpath PUBLIC /INCREMENTAL:NO)
  else()
    target_compile_options(cpath PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(cpath PUBLIC -fsanitize=address,undefined)
  endif()
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(CPATH_BUILD_TOOLS)
  add_executable(cpath_cli apps/cli_main.cpp)
  set_target_properties(cpath_cli PROPERTIES OUTPUT_NAME "cpath")
  target_link_libraries(cpath_cli PRIVATE cpath)
  cpath_apply_warnings(cpath_cli)

  add_executable(cpathd apps/daemon_main.cpp)
  target_link_libraries(cpathd PRIVATE cpath)
  cpath_apply_warnings(cpathd)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(CPATH_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(CPATH_BUILD_TESTS)
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS cpath
  EXPORT CollectivePathPlannerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(CPATH_BUILD_TOOLS)
  install(TARGETS cpath_cli cpathd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/cpath"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")
install(FILES "${CPATH_GENERATED_DIR}/cpath/version.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/cpath)

install(EXPORT CollectivePathPlannerTargets
  FILE CollectivePathPlannerTargets.cmake
  NAMESPACE CollectivePathPlanner::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CollectivePathPlanner)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CollectivePathPlannerConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/CollectivePathPlannerConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CollectivePathPlanner)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/CollectivePathPlannerConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/CollectivePathPlannerConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/CollectivePathPlannerConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CollectivePathPlanner)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/CollectivePathPlanner)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "Collective Path Planner ${PROJECT_VERSION}")
message(STATUS "  build type          : ${CMAKE_BUILD_TYPE}")
message(STATUS "  compiler            : ${CPATH_COMPILER_ID} ${CPATH_COMPILER_VERSION}")
message(STATUS "  warnings as errors  : ${CPATH_WARNINGS_AS_ERRORS}")
message(STATUS "  sanitizers          : ${CPATH_ENABLE_SANITIZERS}")
message(STATUS "  tools               : ${CPATH_BUILD_TOOLS}")
message(STATUS "  tests               : ${CPATH_BUILD_TESTS}")
message(STATUS "  examples            : ${CPATH_BUILD_EXAMPLES}")
