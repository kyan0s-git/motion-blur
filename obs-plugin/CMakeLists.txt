# SPDX-License-Identifier: GPL-2.0-or-later
#
# Configured from the repository root with -DMBLUR_BUILD_OBS_PLUGIN=ON.
#
# libobs is discovered in one of two ways:
#   * find_package(libobs)  - Linux distributions ship working CMake package
#     configs in libobs-dev, and so does the official plugin template.
#   * OBS_SDK_DIR           - a prebuilt SDK assembled from OBS's shipped DLLs
#     and matching headers, which is how Windows is built without compiling
#     OBS itself. See cmake/OBSPrebuilt.cmake.

cmake_minimum_required(VERSION 3.20)

if(NOT TARGET mblur)
  message(FATAL_ERROR
    "obs-plugin must be configured from the repository root "
    "(cmake -S . -B build -DMBLUR_BUILD_OBS_PLUGIN=ON) so it links the "
    "shared blur engine.")
endif()

if(OBS_SDK_DIR)
  include(OBSPrebuilt)
else()
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api QUIET)
endif()

# The frontend API is only needed by the helper that sets the recording
# encoder's frame rate divisor. The filter and both blur modes do not touch
# it, so a platform without it still gets a working plugin - it just cannot
# reconfigure the encoder automatically, which the stub says out loud rather
# than leaving the user to wonder.
if(TARGET OBS::obs-frontend-api)
  set(_mblur_frontend_default ON)
else()
  set(_mblur_frontend_default OFF)
endif()
option(MBLUR_OBS_FRONTEND "Build the frame-rate-divisor frontend helper"
       ${_mblur_frontend_default})

add_library(motion-blur MODULE
  src/plugin-main.c
  src/motion-blur-filter.c
  src/frontend-helper.c)

target_link_libraries(motion-blur PRIVATE OBS::libobs mblur::mblur)
target_include_directories(motion-blur PRIVATE src)

if(MBLUR_OBS_FRONTEND)
  if(NOT TARGET OBS::obs-frontend-api)
    message(FATAL_ERROR
      "MBLUR_OBS_FRONTEND is ON but obs-frontend-api was not found.")
  endif()
  target_link_libraries(motion-blur PRIVATE OBS::obs-frontend-api)
  target_compile_definitions(motion-blur PRIVATE MBLUR_HAVE_OBS_FRONTEND=1)
  message(STATUS "motion-blur: frame rate divisor helper enabled")
else()
  message(STATUS
    "motion-blur: obs-frontend-api unavailable, building without the "
    "automatic frame rate divisor helper")
endif()

set_target_properties(motion-blur PROPERTIES PREFIX "")

# A plugin module links fine with unresolved symbols and then fails to load
# inside OBS, which surfaces to the user as "the filter isn't there" with
# nothing useful in the log. Making the linker reject them turns a runtime
# mystery into a build error. (MSVC already refuses to produce a DLL with
# unresolved externals, so this only needs saying on ELF platforms.)
if(NOT MSVC AND NOT APPLE)
  target_link_options(motion-blur PRIVATE "LINKER:--no-undefined")
endif()

if(MSVC)
  target_compile_options(motion-blur PRIVATE /W4)
else()
  target_compile_options(motion-blur PRIVATE -Wall -Wextra)
endif()

# Data (the .effect and the locale) must land next to the module or
# obs_module_file() cannot find them and the filter refuses to create.
if(DEFINED OBS_PLUGIN_DESTINATION AND DEFINED OBS_DATA_DESTINATION)
  install(TARGETS motion-blur LIBRARY DESTINATION "${OBS_PLUGIN_DESTINATION}")
  install(DIRECTORY data/
          DESTINATION "${OBS_DATA_DESTINATION}/motion-blur")
else()
  message(STATUS
    "motion-blur: OBS install destinations unset; the release workflow "
    "packages the module and data/ into OBS's expected layout.")
endif()
