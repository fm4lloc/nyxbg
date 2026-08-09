/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NyxBG - Image geometry calculations.
 *
 * Copyright (C) 2026 Fernando Magalhães
 *
 * Contact:
 *   fm4lloc@gmail.com
 *   nyx-eco@proton.me
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
/*
 * This module is purely arithmetic. It decides which rectangle of the
 * source image maps onto which rectangle of the output, and nothing else.
 * No rendering, resampling or pixel access occurs here; that is render.c's
 * responsibility. The module allocates nothing and keeps no state.
 */
#ifndef NYXBG_SCALE_H
#define NYXBG_SCALE_H

#include <stdint.h>

/**
 * enum nyx_scale_mode - How the image is mapped onto an output.
 * @NYX_SCALE_FILL: Cover the whole output; crop the overflowing axis.
 * @NYX_SCALE_FIT: Keep the whole image visible; letterbox the remainder.
 * @NYX_SCALE_STRETCH: Cover the whole output, ignoring the aspect ratio.
 * @NYX_SCALE_CENTER: No scaling; centre the image, cropping if it does not
 *                    fit.
 *
 * The mode is chosen once from the command line and never changes while
 * the process runs.
 */
enum nyx_scale_mode {
	NYX_SCALE_FILL,
	NYX_SCALE_FIT,
	NYX_SCALE_STRETCH,
	NYX_SCALE_CENTER
};

/** NYX_SCALE_DEFAULT - The mode used when --mode is not given. */
#define NYX_SCALE_DEFAULT NYX_SCALE_FILL

/**
 * struct nyx_geometry - A source rectangle and where it lands.
 * @src_x: left edge of the source rectangle, in image pixels.
 * @src_y: top edge of the source rectangle, in image pixels.
 * @src_w: width of the source rectangle, in image pixels.
 * @src_h: height of the source rectangle, in image pixels.
 * @dst_x: left edge of the destination rectangle, in buffer pixels.
 * @dst_y: top edge of the destination rectangle, in buffer pixels.
 * @dst_w: width of the destination rectangle, in buffer pixels.
 * @dst_h: height of the destination rectangle, in buffer pixels.
 *
 * Filled in by nyx_scale_compute(). Both rectangles are non-empty and lie
 * entirely inside their respective surfaces, so a caller that trusts the
 * return value does not have to clamp anything.
 */
struct nyx_geometry {
	int32_t src_x, src_y, src_w, src_h;
	int32_t dst_x, dst_y, dst_w, dst_h;
};

int nyx_scale_compute(enum nyx_scale_mode mode,
                      int32_t image_width, int32_t image_height,
                      int32_t output_width, int32_t output_height,
                      struct nyx_geometry *geometry);

int nyx_scale_mode_parse(const char *name, enum nyx_scale_mode *out);

const char *nyx_scale_mode_name(enum nyx_scale_mode mode);

#endif /* NYXBG_SCALE_H */
