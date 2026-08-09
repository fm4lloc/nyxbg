/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NyxBG - wl_buffer creation, pixel upload, damage, attach and commit.
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
 * The renderer owns the shared-memory buffers. Exactly one buffer is alive
 * per layer surface in the steady state; a second one exists only for the
 * brief interval between attaching a replacement and the compositor
 * releasing the old one.
 */
#ifndef NYXBG_RENDER_H
#define NYXBG_RENDER_H

#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

struct nyx_state;
struct nyx_layer;

/**
 * NYX_MAX_BUFFER_DIMENSION - Upper bound on either axis of a
 * shared-memory buffer, and on the surface sizes accepted from the
 * compositor.
 *
 * Sizes and scales arrive from the compositor, which is only semi-trusted:
 * it is a separate process that can send any payload the protocol encoding
 * permits. Every value that comes from it is bounded before it takes part
 * in arithmetic, not after.
 */
#define NYX_MAX_BUFFER_DIMENSION 32767

/*
 * Upper bound on the pixel count of one wl_shm buffer.
 *
 * The per-axis bound alone does not bound the product: a 23170x23170
 * configure passes both axes and asks for a 2 GiB mapping. That is not a
 * crash -- buffer_create() returns NULL if the mapping fails, and the
 * output simply gets no wallpaper -- but a compositor that wants this
 * process to hold two gigabytes can otherwise arrange it, and nothing in
 * the program had an opinion about that.
 *
 * 2^27 pixels is a 512 MiB buffer. The tightest real case is an 8K panel
 * at integer scale 2: 15360x8640 is 132,710,400 pixels, which fits with
 * about 1% to spare. Anything larger than that is a compositor bug or a
 * hostile peer, and gets a diagnostic instead of a mapping.
 *
 * The value coincides with image.c's per-image pixel cap. That is
 * arithmetic, not a shared rule: one bounds a decoded image, the other
 * bounds a surface the compositor asked for, and they are free to diverge.
 */
#define NYX_MAX_BUFFER_PIXELS ((int64_t)134217728)

/*
 * How many shared-memory buffers one layer surface may have alive at once.
 * A buffer is freed as soon as the compositor releases it, so the steady
 * state is one, briefly two while a resize is in flight. The cap exists
 * because a compositor that simply never releases would otherwise make
 * every configure event cost another live mapping, with nothing bounding
 * the total; see nyx_render_layer().
 */
#define NYX_MAX_LAYER_BUFFERS 4

/**
 * struct nyx_buffer - One shared-memory buffer and its wl_buffer.
 * @link: links into &struct nyx_layer.buffers.
 * @wl_buffer: the protocol object. Owned here.
 * @data: the mapping, @size bytes long. Owned here.
 * @size: length of the mapping in bytes.
 * @width: buffer width in buffer pixels.
 * @height: buffer height in buffer pixels.
 * @stride: bytes per row, always @width * 4.
 * @busy: set while the compositor holds the buffer.
 * @doomed: set when the buffer has been superseded and should be destroyed
 *          as soon as it is released.
 * @layer: owning layer, used to retry a dirty render after release.
 *
 * The pixel format is always XRGB8888: opaque, four bytes per pixel,
 * little-endian, which the renderer writes as an explicit B,G,R,X byte
 * order so the result does not depend on the host.
 *
 * @busy and @doomed together are what let a buffer be replaced while the
 * compositor still holds it: the release event finds @doomed set and
 * completes the destruction that nyx_render_layer() started.
 */
struct nyx_buffer {
	struct wl_list link;
	struct nyx_layer *layer;

	struct wl_buffer *wl_buffer;
	void *data;
	size_t size;
	int32_t width, height;
	int32_t stride;

	int busy;
	int doomed;
};

int nyx_render_layer(struct nyx_state *state, struct nyx_layer *layer);

void nyx_buffer_destroy(struct nyx_buffer *buffer);

#endif /* NYXBG_RENDER_H */
