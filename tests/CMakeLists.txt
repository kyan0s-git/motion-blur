# SPDX-License-Identifier: GPL-2.0-or-later
function(mblur_add_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE mblur)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR})
  if(MBLUR_X86)
    target_compile_definitions(${name} PRIVATE MBLUR_X86=1)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

mblur_add_test(test_weights)
mblur_add_test(test_kernels)
mblur_add_test(test_pipeline)

# The CLI is tested through its real binary rather than by linking its
# internals, so the test covers argument handling and exit codes too.
if(MBLUR_BUILD_CLI)
  mblur_add_test(test_cli)
  target_compile_definitions(test_cli PRIVATE
    BLURCLI_PATH="$<TARGET_FILE:blurcli>")
  add_dependencies(test_cli blurcli)
endif()

# The allocation check needs the GNU linker's --wrap. Where it is missing the
# test is skipped rather than silently dropped from the suite.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_LINK_OPTIONS "-Wl,--wrap=malloc")
check_c_source_compiles("
  #include <stdlib.h>
  void *__real_malloc(size_t);
  void *__wrap_malloc(size_t n) { return __real_malloc(n); }
  int main(void) { void *p = malloc(4); free(p); return p != 0 ? 0 : 1; }
" MBLUR_HAVE_LD_WRAP)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if(MBLUR_HAVE_LD_WRAP)
  mblur_add_test(test_noalloc)
  target_link_options(test_noalloc PRIVATE
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
    -Wl,--wrap=posix_memalign)
else()
  message(STATUS "mblur: linker --wrap unavailable, skipping test_noalloc")
endif()
