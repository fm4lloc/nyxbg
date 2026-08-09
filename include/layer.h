/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NyxBG - Layer-shell surface creation and configuration.
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
 * Each layer surface is anchored to all four edges of its output, sits on
 * the background layer, takes no exclusive zone and accepts no input. The
 * compositor reports the surface size through configure; that size is
 * authoritative and is what the renderer draws to.
 */
#ifndef NYXBG_LAYER_H
#define NYXBG_LAYER_H

#include <stdint.h>
#include <wayland-client.h>

struct nyx_output;
struct nyx_buffer;
struct zwlr_layer_surface_v1;

/**
 * struct nyx_layer - The wallpaper surface for one output.
 * @output: the output this surface covers. Not owned.
 * @surface: the wl_surface. Owned here.
 * @layer_surface: the layer-shell role object. Owned here.
 * @width: surface width in logical pixels, from the last configure.
 * @height: surface height in logical pixels, from the last configure.
 * @scale: buffer scale in effect for the currently attached buffer.
 * @buffers: every &struct nyx_buffer allocated for this surface that has
 *           not yet been reclaimed. Owned here.
 * @buffer: the currently attached buffer; also present on @buffers, or NULL
 *         after its release until the next successful render.
 * @configured: set once the compositor has sent a usable configure.
 * @dirty: set when the surface owes the compositor a frame it has not yet
 *         successfully produced. A configure sets it; a successful render
 *         clears it. Without it a render that failed once would never be
 *         retried, because a repeated identical configure changes neither
 *         the size nor the scale and would find nothing to do.
 *
 * @buffers normally holds exactly one entry. A second appears for the
 * interval between attaching a replacement and the compositor releasing
 * the old one, and keeping the list means shutdown can reclaim a buffer
 * the compositor is still holding -- otherwise unreachable once it has
 * been replaced as the attached one.
 */
struct nyx_layer {
	struct nyx_output *output;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	int32_t width, height;
	int32_t scale;

	struct wl_list buffers;
	struct nyx_buffer *buffer;
	int configured;
	int dirty;
};

struct nyx_layer *nyx_layer_create(struct nyx_output *output);

void nyx_layer_destroy(struct nyx_layer *layer);

void nyx_layer_redraw(struct nyx_layer *layer);

#endif /* NYXBG_LAYER_H */
