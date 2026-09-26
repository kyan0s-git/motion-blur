# SPDX-License-Identifier: GPL-2.0-or-later
#
# Builds OBS::libobs and OBS::obs-frontend-api from a prebuilt SDK directory
# instead of a real libobs installation.
#
# Why this exists: on Linux, distributions ship libobs-dev with working CMake
# package configs and find_package() is the whole story. Windows has no such
# package, and building OBS from source to get one means pulling obs-deps and
# Qt for a library our plugin links but does not need to compile.
#
# The shortcut is that OBS already ships the exact DLLs the plugin will load
# at runtime. Headers come from the matching source tag, and import libraries
# are generated from those DLLs' own export tables, so the link is against
# precisely what will be loaded. See .github/scripts/make-obs-sdk.ps1.
#
# Expected layout of OBS_SDK_DIR:
#   include/obs-module.h, include/obs.h, include/graphics/, include/util/, ...
#   include/obs-frontend-api.h
#   lib/obs.lib
#   lib/obs-frontend-api.lib

if(NOT OBS_SDK_DIR)
  message(FATAL_ERROR "OBSPrebuilt: OBS_SDK_DIR is not set")
endif()

get_filename_component(_obs_sdk "${OBS_SDK_DIR}" ABSOLUTE)

if(NOT EXISTS "${_obs_sdk}/include/obs-module.h")
  message(FATAL_ERROR
    "OBSPrebuilt: ${_obs_sdk}/include/obs-module.h not found. "
    "OBS_SDK_DIR must point at an assembled SDK, not an OBS install.")
endif()

# OBS's own import library is called obs.lib; accept a couple of spellings so
# an SDK assembled by hand still works.
find_library(OBS_LIBOBS_IMPLIB
  NAMES obs libobs
  PATHS "${_obs_sdk}/lib"
  NO_DEFAULT_PATH
  REQUIRED)

add_library(OBS::libobs UNKNOWN IMPORTED)
set_target_properties(OBS::libobs PROPERTIES
  IMPORTED_LOCATION "${OBS_LIBOBS_IMPLIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${_obs_sdk}/include")

message(STATUS "OBSPrebuilt: libobs -> ${OBS_LIBOBS_IMPLIB}")

find_library(OBS_FRONTEND_IMPLIB
  NAMES obs-frontend-api
  PATHS "${_obs_sdk}/lib"
  NO_DEFAULT_PATH)

if(OBS_FRONTEND_IMPLIB AND EXISTS "${_obs_sdk}/include/obs-frontend-api.h")
  add_library(OBS::obs-frontend-api UNKNOWN IMPORTED)
  set_target_properties(OBS::obs-frontend-api PROPERTIES
    IMPORTED_LOCATION "${OBS_FRONTEND_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${_obs_sdk}/include")
  message(STATUS "OBSPrebuilt: obs-frontend-api -> ${OBS_FRONTEND_IMPLIB}")
else()
  message(STATUS "OBSPrebuilt: obs-frontend-api not present in the SDK")
endif()
