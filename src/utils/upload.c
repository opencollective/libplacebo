/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */

#include <strings.h>

#include "context.h"
#include "common.h"

struct comp {
    int order; // e.g. 0, 1, 2, 3 for RGBA
    int size;  // size in bits
    int shift; // bit-shift / offset in bits
};

static int compare_comp(const void *pa, const void *pb)
{
    const struct comp *a = pa, *b = pb;

    // Move all of the components with a size of 0 to the end, so they can
    // be ignored outright
    if (a->size && !b->size)
        return -1;
    if (b->size && !a->size)
        return 1;

    // Otherwise, just compare based on the shift
    return PL_CMP(a->shift, b->shift);
}

void pl_plane_data_from_mask(struct pl_plane_data *data, uint64_t mask[4])
{
    struct comp comps[4] = { {0}, {1}, {2}, {3} };

    for (int i = 0; i < PL_ARRAY_SIZE(comps); i++) {
        comps[i].size = __builtin_popcount(mask[i]);
        comps[i].shift = PL_MAX(0, __builtin_ffsll(mask[i]) - 1);
    }

    // Sort the components by shift
    qsort(comps, PL_ARRAY_SIZE(comps), sizeof(struct comp), compare_comp);

    // Generate the resulting component size/pad/map
    int offset = 0;
    for (int i = 0; i < PL_ARRAY_SIZE(comps); i++)  {
        if (!comps[i].size)
            return;

        assert(comps[i].shift >= offset);
        data->component_size[i] = comps[i].size;
        data->component_pad[i] = comps[i].shift - offset;
        data->component_map[i] = comps[i].order;
        offset += data->component_size[i] + data->component_pad[i];
    }
}

const struct ra_fmt *pl_plane_find_fmt(const struct ra *ra, int out_map[4],
                                       const struct pl_plane_data *data)
{
    // Count the number of components and initialize out_map
    int num = 0;
    for (int i = 0; i < PL_ARRAY_SIZE(data->component_size); i++) {
        out_map[i] = -1;
        if (data->component_size[i])
            num = i+1;
    }

    for (int n = 0; n < ra->num_formats; n++) {
        const struct ra_fmt *fmt = ra->formats[n];
        if (fmt->opaque || fmt->num_components < num)
            continue;
        if (fmt->type != data->type || fmt->texel_size != data->pixel_stride)
            continue;
        if (!(fmt->caps & RA_FMT_CAP_SAMPLEABLE))
            continue;

        int idx = 0;

        // Try mapping all pl_plane_data components to texture components
        for (int i = 0; i < num; i++) {
            // If there's padding we have to map it to an unused physical
            // component first
            int pad = data->component_pad[i];
            if (pad && (idx >= 4 || fmt->host_bits[idx++] != pad))
                goto next_fmt;

            // Otherwise, try and match this component
            int size = data->component_size[i];
            if (size && (idx >= 4 || fmt->host_bits[idx] != size))
                goto next_fmt;
            out_map[idx++] = data->component_map[i];
        }

        return fmt;

next_fmt: ; // acts as `continue`
    }

    return NULL;
}

bool pl_upload_plane(const struct ra *ra, struct pl_plane *plane,
                     const struct pl_plane_data *data)
{
    size_t row_stride = PL_DEF(data->row_stride, data->pixel_stride * data->width);
    unsigned int stride_texels = row_stride / data->pixel_stride;
    if (stride_texels * data->pixel_stride != row_stride) {
        PL_ERR(ra, "data->row_stride must be a multiple of data->pixel_stride!");
        return false;
    }

    int out_map[4];
    const struct ra_fmt *fmt = pl_plane_find_fmt(ra, out_map, data);
    if (!fmt) {
        PL_ERR(ra, "Failed picking any compatible texture format for a plane!");
        return false;

        // TODO: try soft-converting to a supported format using e.g zimg?
    }

    struct ra_tex_params params = {0};
    if (plane->texture)
        params = plane->texture->params;

    if (params.format != fmt || !params.sampleable || !params.host_writable ||
        params.w != data->width || params.h != data->height)
    {
        PL_INFO(ra, "Reinitializing %dx%d plane texture (size or fmt change)",
                data->width, data->height);

        ra_tex_destroy(ra, &plane->texture);
        plane->texture = ra_tex_create(ra, &(struct ra_tex_params) {
            .w = data->width,
            .h = data->height,
            .format = fmt,
            .sampleable = true,
            .host_writable = true,
            .blit_src = !!(fmt->caps & RA_FMT_CAP_BLITTABLE),
            .address_mode = RA_TEX_ADDRESS_CLAMP,
            .sample_mode = (fmt->caps & RA_FMT_CAP_LINEAR)
                                ? RA_TEX_SAMPLE_LINEAR
                                : RA_TEX_SAMPLE_NEAREST,
        });

        if (!plane->texture) {
            PL_ERR(ra, "Failed initializing plane texture!");
            return false;
        }
    }

    for (int i = 0; i < PL_ARRAY_SIZE(out_map); i++) {
        plane->component_mapping[i] = out_map[i];
        if (out_map[i] >= 0)
            plane->components = i+1;
    }

    return ra_tex_upload(ra, &(struct ra_tex_transfer_params) {
        .tex      = plane->texture,
        .stride_w = stride_texels,
        .ptr      = (void *) data->pixels,
    });
}
