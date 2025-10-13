# SPDX-FileCopyrightText: 2025 CERN
# SPDX-License-Identifier: GPL-2.0-only

cmake_minimum_required(VERSION 3.20)
project(adaptyst-linuxperf
  DESCRIPTION "The CPU Linux-perf-based hardware module for Adaptyst")

option(PERF "Compile patched \"perf\"" ON)
option(ROOFLINE "Compile adaptyst-linuxperf with cache-aware roofline support (requires GCC)" ON)
set(PERF_TAG "dev-20250408" CACHE STRING "Patched \"perf\" git tag which should be used for setting up \"perf\"")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(adaptyst REQUIRED)

message(STATUS "Adaptyst module path: ${ADAPTYST_MODULE_PATH}")
set(INSTALL_PATH "${ADAPTYST_MODULE_PATH}" CACHE STRING "Path where adaptyst-linuxperf should be installed")

add_library(linuxperf SHARED
  src/linuxperf.cpp
  src/linuxperf_profiling.cpp)

target_compile_definitions(linuxperf PRIVATE MODULE_PATH="${INSTALL_PATH}/linuxperf")

find_package(PkgConfig REQUIRED)
pkg_check_modules(NUMA numa)
if(NUMA_FOUND)
  message(STATUS "Found libnuma: ${NUMA_LINK_LIBRARIES}  ${NUMA_INCLUDE_DIRS}")
  target_compile_definitions(linuxperf PRIVATE LIBNUMA_AVAILABLE)
  target_include_directories(linuxperf PRIVATE ${NUMA_INCLUDE_DIRS})
  target_link_libraries(linuxperf PUBLIC ${NUMA_LINK_LIBRARIES})
else()
  message(STATUS "numa not found, compiling without libnuma support")
endif()

if(ROOFLINE)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_definitions(linuxperf PRIVATE ADAPTYST_ROOFLINE)
  else()
    message(FATAL_ERROR "Cache-aware roofline support requires adaptyst-linuxperf to be compiled with GCC. Change your compiler or set ROOFLINE to OFF.")
  endif()
endif()

target_include_directories(linuxperf PRIVATE src)
target_link_libraries(linuxperf PUBLIC adaptyst::adaptyst)

# Patched "perf" setup
if(PERF)
  include(ExternalProject)

  if(PERF_REPOSITORY_DIR)
    ExternalProject_Add(perf
      SOURCE_DIR ${PERF_REPOSITORY_DIR}
      CONFIGURE_COMMAND ""
      BUILD_IN_SOURCE TRUE
      BUILD_COMMAND sh -c "make -C tools/perf install BUILD_BPF_SKEL=1 prefix=\"${CMAKE_BINARY_DIR}/perf-install\""
      INSTALL_COMMAND "")
  else()
    ExternalProject_Add(perf
      SOURCE_DIR linux
      GIT_REPOSITORY https://gitlab.cern.ch/adaptyst/linux
      GIT_TAG ${PERF_TAG}
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
      CONFIGURE_COMMAND ""
      BUILD_IN_SOURCE TRUE
      BUILD_COMMAND sh -c "make -C tools/perf install BUILD_BPF_SKEL=1 prefix=\"${CMAKE_BINARY_DIR}/perf-install\""
      INSTALL_COMMAND "")
  endif()

  install(DIRECTORY ${CMAKE_BINARY_DIR}/perf-install/ DESTINATION ${INSTALL_PATH}/linuxperf/perf USE_SOURCE_PERMISSIONS)
endif()

install(TARGETS linuxperf LIBRARY DESTINATION ${INSTALL_PATH}/linuxperf)
install(FILES
  src/scripts/event-handler.py
  src/scripts/cxxfilt.py
  DESTINATION ${INSTALL_PATH}/linuxperf)
