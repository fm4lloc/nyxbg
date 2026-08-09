/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NyxBG - Output discovery, hotplug, resolution and scale.
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
 * One struct nyx_output exists per wl_output global advertised by the
 * compositor. The layer surface attached to it is created once the
 * output's initial property burst has been delivered.
 */
#ifndef NYXBG_OUTPUT_H
#define NYXBG_OUTPUT_H

#include <stdint.h>
#include <wayland-client.h>

struct nyx_state;
struct nyx_layer;

/**
 * NYX_MAX_OUTPUT_SCALE - Ceiling on the integer buffer scale accepted from
 * wl_output.
 *
 * The scale multiplies the surface size to give the buffer size, so an
 * unbounded value here would be an unbounded multiplication in the
 * renderer. Nothing ships above 3; this leaves room to spare while keeping
 * that product far from any boundary.
 */
#define NYX_MAX_OUTPUT_SCALE 16

/**
 * struct nyx_output - One monitor the compositor has advertised.
 * @link: links into &struct nyx_state.outputs.
 * @state: the application state this output belongs to. Not owned.
 * @wl_output: the bound protocol object. Owned here.
 * @global_name: registry name, needed to match a global_remove event.
 * @width: current mode width in physical pixels.
 * @height: current mode height in physical pixels.
 * @scale: integer buffer scale, at least 1 and at most
 *         %NYX_MAX_OUTPUT_SCALE.
 * @transform: the output's rotation or flip, as a wl_output transform.
 * @name: connector name such as "HDMI-A-1". Owned here; NULL below
 *        wl_output version 4.
 * @layer: the wallpaper surface for this output. Owned here; NULL until
 *         the layer shell is available and the output is complete.
 * @done_seen: set once the compositor has finished describing the output.
 * @layer_closed: set once the compositor has closed this output's layer
 *                surface. The surface is never recreated afterwards, so
 *                a later wl_output.done cannot be used to make the client
 *                rebuild a surface the compositor just asked it to drop.
 *
 * @width, @height and @transform are used for diagnostics and for
 * detecting change. The size the renderer actually draws to is the one
 * from the layer surface's configure event, not these.
 */
struct nyx_output {
	struct wl_list link;
	struct nyx_state *state;

	struct wl_output *wl_output;
	uint32_t global_name;

	int32_t width, height;
	int32_t scale;
	int32_t transform;
	char *name;

	struct nyx_layer *layer;
	int done_seen;
	int layer_closed;
};

void nyx_output_add(struct nyx_state *state, uint32_t global_name,
                    uint32_t version);

void nyx_output_remove(struct nyx_state *state, uint32_t global_name);

void nyx_output_destroy(struct nyx_output *output);

void nyx_output_destroy_all(struct nyx_state *state);

void nyx_output_create_surfaces(struct nyx_state *state);

#endif /* NYXBG_OUTPUT_H */
