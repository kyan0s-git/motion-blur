# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(blurcli main.c options.c)
target_link_libraries(blurcli PRIVATE mblur)
target_include_directories(blurcli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
