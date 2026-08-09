/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NyxBG - Image decoding.
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
 * Decodes PNG and JPEG files into a single canonical in-memory format:
 * 8 bits per channel, RGBA byte order, non-premultiplied alpha, tightly
 * packed rows. This is the "RGBA Buffer" stage of the pipeline; no
 * scaling, positioning or Wayland interaction happens here.
 */
#ifndef NYXBG_IMAGE_H
#define NYXBG_IMAGE_H

#include <stdint.h>

/**
 * struct nyx_image - A decoded image in the pipeline's canonical format.
 * @width: width in pixels; always positive and bounded by the decoder.
 * @height: height in pixels; always positive and bounded by the decoder.
 * @pixels: @width * @height * 4 bytes, row-major, R,G,B,A byte order,
 *          8 bits per channel, alpha not premultiplied, rows tightly
 *          packed with no padding.
 *
 * Allocated by nyx_image_load() and released by nyx_image_destroy(). The
 * state owns exactly one of these at a time; SIGHUP replaces it.
 */
struct nyx_image {
	int32_t width;
	int32_t height;
	unsigned char *pixels;
};

struct nyx_image *nyx_image_load(const char *path);

void nyx_image_destroy(struct nyx_image *image);

#endif /* NYXBG_IMAGE_H */
