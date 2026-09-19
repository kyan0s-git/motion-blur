# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.20)
project(motion-blur
        VERSION 0.1.0
        DESCRIPTION "Fast temporal motion blur for OBS and offline video"
        LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(MBLUR_BUILD_TESTS "Build the test suite" ON)
option(MBLUR_BUILD_BENCH "Build the benchmark harness" ON)
option(MBLUR_BUILD_CLI "Build the offline blurcli tool" ON)
option(MBLUR_BUILD_OBS_PLUGIN "Build the OBS Studio plugin" OFF)

include(CheckCSourceCompiles)

# --- target architecture -------------------------------------------------
set(MBLUR_X86 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$")
  set(MBLUR_X86 ON)
endif()

# --- core library --------------------------------------------------------
set(MBLUR_CORE_SOURCES
    src/core/layout.c
    src/core/weights.c
    src/core/color.c
    src/core/names.c
    src/core/pipeline.c
    src/core/threadpool.c
    src/cpu/blend_scalar.c
    src/cpu/dispatch.c)

if(MBLUR_X86)
  list(APPEND MBLUR_CORE_SOURCES
       src/cpu/blend_sse2.c
       src/cpu/blend_avx2.c
       src/cpu/blend_avx512.c)
endif()

add_library(mblur STATIC ${MBLUR_CORE_SOURCES})
add_library(mblur::mblur ALIAS mblur)

target_include_directories(mblur
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MBLUR_X86)
  target_compile_definitions(mblur PRIVATE MBLUR_X86=1)
endif()

# Each vectorised translation unit is compiled for its own instruction set;
# the ISA is selected at runtime, so nothing here raises the baseline the
# library as a whole requires.
if(MBLUR_X86 AND NOT MSVC)
  set_source_files_properties(src/cpu/blend_sse2.c   PROPERTIES COMPILE_OPTIONS "-msse2")
  set_source_files_properties(src/cpu/blend_avx2.c   PROPERTIES COMPILE_OPTIONS "-mavx2")
  set_source_files_properties(src/cpu/blend_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512bw")
elseif(MBLUR_X86 AND MSVC)
  set_source_files_properties(src/cpu/blend_avx2.c   PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  set_source_files_properties(src/cpu/blend_avx512.c PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
endif()

if(MSVC)
  target_compile_options(mblur PRIVATE /W4)
else()
  target_compile_options(mblur PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
    -Wno-unused-parameter)
  target_link_libraries(mblur PUBLIC m)
  find_package(Threads REQUIRED)
  target_link_libraries(mblur PUBLIC Threads::Threads)
endif()

if(MBLUR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(MBLUR_BUILD_BENCH)
  add_subdirectory(bench)
endif()
if(MBLUR_BUILD_CLI)
  add_subdirectory(apps/blurcli)
endif()
if(MBLUR_BUILD_OBS_PLUGIN)
  add_subdirectory(obs-plugin)
endif()
