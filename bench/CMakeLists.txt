# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(mblur_bench bench.c)
target_link_libraries(mblur_bench PRIVATE mblur)
