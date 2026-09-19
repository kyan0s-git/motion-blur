/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Enum <-> string, for config files, CLI flags and OBS property lists.
 * Parsers return -1 on an unknown name rather than a default, so a typo in
 * a config surfaces as an error instead of silently changing the look.
 */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>

#define MBLUR_STRINGIFY2(x) #x
#define MBLUR_STRINGIFY(x) MBLUR_STRINGIFY2(x)

static const char *const k_weighting[MBLUR_W_COUNT] = {
    "equal",      "gaussian",  "gaussian_sym", "gaussian_reverse", "pyramid",
    "vegas",      "ascending", "descending",   "custom"};

static const char *const k_format[MBLUR_FMT_COUNT] = {"rgba8", "bgra8", "nv12",
                                                      "i420"};

static const char *const k_transfer[MBLUR_TRC_COUNT] = {"srgb", "bt709",
                                                        "linear"};

static const char *const k_mode[2] = {"rolling", "decimate"};

static int lookup(const char *const *table, int count, const char *name)
{
    if (!name)
        return -1;
    for (int i = 0; i < count; i++) {
        const char *a = table[i];
        const char *b = name;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (cb >= 'A' && cb <= 'Z')
                cb = (char)(cb - 'A' + 'a');
            if (cb == '-')
                cb = '_';
            if (ca != cb)
                break;
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0')
            return i;
    }
    return -1;
}

const char *mblur_weighting_name(mblur_weighting w)
{
    return (w < MBLUR_W_COUNT) ? k_weighting[w] : "?";
}
int mblur_weighting_parse(const char *name)
{
    return lookup(k_weighting, MBLUR_W_COUNT, name);
}
const char *mblur_format_name(mblur_format f)
{
    return (f < MBLUR_FMT_COUNT) ? k_format[f] : "?";
}
int mblur_format_parse(const char *name)
{
    return lookup(k_format, MBLUR_FMT_COUNT, name);
}
const char *mblur_transfer_name(mblur_transfer t)
{
    return (t < MBLUR_TRC_COUNT) ? k_transfer[t] : "?";
}
int mblur_transfer_parse(const char *name)
{
    return lookup(k_transfer, MBLUR_TRC_COUNT, name);
}
const char *mblur_mode_name(mblur_mode m)
{
    return (m == MBLUR_MODE_DECIMATE) ? k_mode[1] : k_mode[0];
}
int mblur_mode_parse(const char *name)
{
    return lookup(k_mode, 2, name);
}

const char *mblur_version_string(void)
{
    return MBLUR_STRINGIFY(MBLUR_VERSION_MAJOR) "." MBLUR_STRINGIFY(
        MBLUR_VERSION_MINOR) "." MBLUR_STRINGIFY(MBLUR_VERSION_PATCH);
}

const char *mblur_status_string(mblur_status st)
{
    switch (st) {
    case MBLUR_OK:
        return "ok";
    case MBLUR_NO_OUTPUT:
        return "no output yet";
    case MBLUR_ERR_INVALID:
        return "invalid argument";
    case MBLUR_ERR_MEMORY:
        return "out of memory";
    case MBLUR_ERR_UNSUPPORTED:
        return "unsupported";
    case MBLUR_ERR_BACKEND:
        return "backend failure";
    }
    return "unknown";
}

const char *mblur_backend_name(mblur_backend b)
{
    switch (b) {
    case MBLUR_BACKEND_AUTO:
        return "auto";
    case MBLUR_BACKEND_CPU_SCALAR:
        return "scalar";
    case MBLUR_BACKEND_CPU_SSE2:
        return "sse2";
    case MBLUR_BACKEND_CPU_AVX2:
        return "avx2";
    case MBLUR_BACKEND_CPU_AVX512:
        return "avx512";
    case MBLUR_BACKEND_D3D11:
        return "d3d11";
    default:
        return "?";
    }
}

int mblur_weights_parse_csv(const char *csv, float *out, uint32_t max_out)
{
    if (!csv || !out || max_out == 0)
        return -1;

    uint32_t count = 0;
    const char *p = csv;

    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        if (count >= max_out)
            return -1;

        char *endp = NULL;
        double v = strtod(p, &endp);
        if (endp == p)
            return -1; /* not a number: report it, do not skip it */
        if (v < 0.0)
            return -1;
        out[count++] = (float)v;
        p = endp;
    }

    return (count > 0) ? (int)count : -1;
}
