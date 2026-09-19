# SPDX-License-Identifier: GPL-2.0-or-later
#
# Built either standalone (the obs-plugintemplate way, with buildspec.json
# supplying prebuilt deps) or as part of this repository's top-level build
# with -DMBLUR_BUILD_OBS_PLUGIN=ON and libobs already discoverable.

cmake_minimum_required(VERSION 3.20)

if(NOT TARGET mblur)
  message(FATAL_ERROR
    "obs-plugin must be configured from the repository root "
    "(cmake -S . -B build -DMBLUR_BUILD_OBS_PLUGIN=ON) so it links the "
    "shared blur engine.")
endif()

find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)

add_library(motion-blur MODULE
  src/plugin-main.c
  src/motion-blur-filter.c
  src/frontend-helper.c)

target_link_libraries(motion-blur PRIVATE
  OBS::libobs
  OBS::obs-frontend-api
  mblur::mblur)

target_include_directories(motion-blur PRIVATE src)

set_target_properties(motion-blur PROPERTIES PREFIX "")

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
    "motion-blur: OBS install destinations unset; copy data/ next to the "
    "module by hand, or build through the plugin template for packaging.")
endif()
