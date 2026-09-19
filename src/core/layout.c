/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "internal.h"

mblur_status mblur_layout_init(mblur_layout *lay, mblur_format fmt,
                               uint32_t width, uint32_t height)
{
    memset(lay, 0, sizeof(*lay));

    switch (fmt) {
    case MBLUR_FMT_RGBA8:
    case MBLUR_FMT_BGRA8:
        /* Alpha sits at byte 3 in both; the difference is only which of the
         * first three bytes is red, and the blend treats them identically. */
        lay->plane[0] = (mblur_plane_info){width * 4u, height, 4u, 3};
        lay->plane_count = 1;
        break;

    case MBLUR_FMT_NV12:
        if ((width | height) & 1u)
            return MBLUR_ERR_INVALID;
        lay->plane[0] = (mblur_plane_info){width, height, 1u, -1};
        lay->plane[1] = (mblur_plane_info){width, height / 2u, 2u, -1};
        lay->plane_count = 2;
        lay->is_yuv = 1;
        break;

    case MBLUR_FMT_I420:
        if ((width | height) & 1u)
            return MBLUR_ERR_INVALID;
        lay->plane[0] = (mblur_plane_info){width, height, 1u, -1};
        lay->plane[1] = (mblur_plane_info){width / 2u, height / 2u, 1u, -1};
        lay->plane[2] = lay->plane[1];
        lay->plane_count = 3;
        lay->is_yuv = 1;
        break;

    default:
        return MBLUR_ERR_INVALID;
    }

    for (uint32_t i = 0; i < lay->plane_count; i++)
        lay->total_bytes += (size_t)lay->plane[i].bytes_wide * lay->plane[i].rows;

    return MBLUR_OK;
}
