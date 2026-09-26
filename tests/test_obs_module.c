/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Loadability check for the OBS plugin.
 *
 * OBS loads plugins with dlopen/LoadLibrary and looks up three exports. If
 * any of them is missing, or the module has an unresolved symbol, the load
 * fails and OBS reports it at LOG_DEBUG - so the user's symptom is "the
 * filter isn't in the list" with an apparently clean log. That is a
 * miserable thing to debug and a trivial thing to test for, so it is tested
 * for here.
 *
 * This does not run the plugin: doing that needs a graphics context and a
 * real OBS process. It proves the module is loadable and complete, which is
 * the part that can be checked without one.
 */
#include "mbtest.h"

#include <dlfcn.h>

#ifndef MOTION_BLUR_MODULE_PATH
#error "MOTION_BLUR_MODULE_PATH must name the built module"
#endif

int main(void)
{
    printf("test_obs_module (%s)\n", MOTION_BLUR_MODULE_PATH);

    MBT_CASE("the module loads with all symbols resolved");
    void *h = dlopen(MOTION_BLUR_MODULE_PATH, RTLD_NOW);
    /* RTLD_NOW forces every relocation up front, so a call to a libobs
     * function that does not exist in this OBS version fails here rather
     * than when a user first applies the filter. */
    MBT_CHECK(h != NULL, "dlopen failed: %s", dlerror());
    MBT_DONE();

    if (!h)
        return mbtest_report("test_obs_module");

    MBT_CASE("it exports what OBS looks up");
    {
        static const char *const required[] = {
            "obs_module_load",
            "obs_module_ver",
            "obs_module_set_pointer",
        };
        for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++)
            MBT_CHECK(dlsym(h, required[i]) != NULL, "%s is missing",
                      required[i]);
    }
    MBT_DONE();

    MBT_CASE("it reports a libobs version OBS will accept");
    {
        uint32_t (*module_ver)(void) =
            (uint32_t(*)(void))dlsym(h, "obs_module_ver");
        MBT_CHECK(module_ver != NULL, "obs_module_ver is missing");
        if (module_ver) {
            const uint32_t v = module_ver();
            const uint32_t major = v >> 24;
            const uint32_t minor = (v >> 16) & 0xFFu;

            printf("\n    built against libobs %u.%u.%u ", major, minor,
                   v & 0xFFFFu);

            /* libobs refuses a plugin whose major.minor exceeds the host's,
             * masking off the patch. Building against anything newer than
             * the floor we advertise would silently stop the plugin loading
             * on the older releases the README promises to support. */
            MBT_CHECK(major == 30 && minor == 0,
                      "expected the OBS 30.0 floor, got %u.%u - this build "
                      "will not load on OBS releases older than that",
                      major, minor);
        }
    }
    MBT_DONE();

    dlclose(h);
    return mbtest_report("test_obs_module");
}
