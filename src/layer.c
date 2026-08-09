// SPDX-License-Identifier: GPL-3.0-or-later
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
 * The surface is anchored to all four edges with a size of 0x0, which is
 * the layer-shell idiom for "give me the whole output"; the compositor
 * answers with the real size in its configure event. An exclusive zone of
 * -1 asks to be laid out ignoring panels and docks, so a wallpaper still
 * covers the full output when something else has reserved space.
 */

#include <stdlib.h>

#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "layer.h"
#include "output.h"
#include "render.h"
#include "util.h"
#include "wayland.h"

#define ANCHOR_ALL \
	(ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | \
	 ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | \
	 ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | \
	 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT)

/**
 * output_label() - Name an output for a diagnostic.
 * @output: output to name.
 *
 * Return: The connector name, or "?" when the compositor is too old to
 * provide one. Never NULL, so the result can be passed straight to a "%s"
 * conversion.
 */
static const char *
output_label(const struct nyx_output *output)
{
	return output->name != NULL ? output->name : "?";
}

/* --------------------------------------------------------------------- */
/* Listener                                                               */
/* --------------------------------------------------------------------- */

/**
 * layer_surface_handle_configure() - Accept a surface size and redraw.
 * @data: the struct nyx_layer.
 * @layer_surface: the role object that sent the event.
 * @serial: serial to acknowledge.
 * @width: surface width in logical pixels.
 * @height: surface height in logical pixels.
 *
 * The configure is acknowledged first and unconditionally, because the
 * protocol requires it even when the size is then rejected.
 *
 * The sizes arrive as unsigned 32-bit values. Storing one above INT32_MAX
 * would make it a negative int32_t, and a merely huge one would reach the
 * renderer's arithmetic; both are bounded here, at the point they enter
 * the program. A compositor that sends either gets a diagnostic and no
 * wallpaper, not a crash.
 *
 * Redrawing happens only when something that affects the pixels changed --
 * the size, the buffer scale, or this being the first configure. A
 * compositor is free to send configure repeatedly with the same size, and
 * re-rendering each time would be wasted work.
 */
static void
layer_surface_handle_configure(void *data,
                               struct zwlr_layer_surface_v1 *layer_surface,
                               uint32_t serial, uint32_t width, uint32_t height)
{
	struct nyx_layer *layer = data;
	int size_changed;

	zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

	if (width == 0 || height == 0) {
		nyx_log("compositor configured output %s with a zero size",
		        output_label(layer->output));
		return;
	}

	if (width > NYX_MAX_BUFFER_DIMENSION || height > NYX_MAX_BUFFER_DIMENSION) {
		nyx_log("ignoring implausible configure of %ux%u on output %s",
		        width, height, output_label(layer->output));
		return;
	}

	size_changed = (layer->width != (int32_t)width ||
	                layer->height != (int32_t)height);

	layer->width = (int32_t)width;
	layer->height = (int32_t)height;

	if (!layer->configured || size_changed ||
	    layer->scale != layer->output->scale)
		layer->dirty = 1;

	if (layer->dirty) {
		layer->configured = 1;
		nyx_layer_redraw(layer);
	}
}

/**
 * layer_surface_handle_closed() - Give up a surface the compositor has
 *                                 closed.
 * @data: the struct nyx_layer.
 * @layer_surface: the role object that sent the event. Unused; the layer
 *                 owns it and destroys it below.
 *
 * The compositor closes a layer surface when its output goes away, or
 * because it has decided the surface should not exist. Either way the
 * surface is not recreated: the request is not a suggestion.
 *
 * The output's pointer is cleared before the layer is destroyed, so no
 * dangling reference is left behind. The output itself survives; if it is
 * genuinely gone, the registry's global_remove event will say so.
 */
static void
layer_surface_handle_closed(void *data,
                            struct zwlr_layer_surface_v1 *layer_surface)
{
	struct nyx_layer *layer = data;
	struct nyx_output *output = layer->output;

	(void)layer_surface;

	nyx_log("compositor closed the layer surface on output %s",
	        output_label(output));

	output->layer = NULL;
	output->layer_closed = 1;
	nyx_layer_destroy(layer);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	layer_surface_handle_configure,
	layer_surface_handle_closed
};

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

/**
 * nyx_layer_create() - Create the wallpaper surface for an output.
 * @output: output to cover. Its state must have a compositor and a layer
 *          shell bound.
 *
 * The surface is configured as a background: anchored to all four edges at
 * size 0x0 so the compositor picks the size, exclusive zone -1 so panels
 * do not shrink it, and no keyboard interactivity.
 *
 * The input region is set empty, so pointer and touch events fall through
 * to whatever is underneath. A failure to create that region is not fatal;
 * the wallpaper would merely swallow clicks, which is worth less than
 * refusing to draw at all.
 *
 * The final commit carries no buffer. That is what asks the compositor for
 * the initial configure, which is the first point at which the surface's
 * size is known.
 *
 * Return: The new layer on success, NULL on failure. Diagnostics have
 * already been printed on failure.
 */
struct nyx_layer *
nyx_layer_create(struct nyx_output *output)
{
	struct nyx_state *state = output->state;
	struct nyx_layer *layer;
	struct wl_region *empty;

	if (state->compositor == NULL || state->layer_shell == NULL) {
		/* Unreachable: nyx_wayland_connect() refuses to return without
		 * both globals. Kept as defence in depth, and diagnosed rather
		 * than silent, because a defensive branch that fires is an
		 * internal bug and a silent internal bug is the worst kind. */
		nyx_log("cannot create a layer surface for output %s: "
		        "the compositor or the layer shell is missing",
		        output_label(output));
		return NULL;
	}

	layer = nyx_alloc(1, sizeof(*layer));
	layer->output = output;
	layer->scale = output->scale;
	wl_list_init(&layer->buffers);

	layer->surface = wl_compositor_create_surface(state->compositor);
	if (layer->surface == NULL) {
		nyx_log("cannot create a surface for output %s",
		        output_label(output));
		free(layer);
		return NULL;
	}

	empty = wl_compositor_create_region(state->compositor);
	if (empty != NULL) {
		wl_surface_set_input_region(layer->surface, empty);
		wl_region_destroy(empty);
	}

	layer->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
	    state->layer_shell, layer->surface, output->wl_output,
	    ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "wallpaper");
	if (layer->layer_surface == NULL) {
		nyx_log("cannot create a layer surface for output %s",
		        output_label(output));
		wl_surface_destroy(layer->surface);
		free(layer);
		return NULL;
	}

	zwlr_layer_surface_v1_set_size(layer->layer_surface, 0, 0);
	zwlr_layer_surface_v1_set_anchor(layer->layer_surface, ANCHOR_ALL);
	zwlr_layer_surface_v1_set_exclusive_zone(layer->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer->layer_surface,
	    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);

	zwlr_layer_surface_v1_add_listener(layer->layer_surface,
	                                   &layer_surface_listener, layer);

	/* Commit with no buffer attached: this is the request for the
	 * initial configure that will tell us how big the surface is. */
	wl_surface_commit(layer->surface);

	nyx_debug("layer surface created for output %s", output_label(output));

	return layer;
}

/**
 * nyx_layer_destroy() - Release a layer surface and everything it owns.
 * @layer: layer to release. May be NULL, in which case nothing happens.
 *
 * The surfaces go first. Once they are gone the compositor holds no
 * reference to any of the buffers, so every buffer on the list can be
 * destroyed unconditionally -- including one still marked busy, whose
 * release event will now never arrive.
 */
void
nyx_layer_destroy(struct nyx_layer *layer)
{
	struct nyx_buffer *buffer, *tmp;

	if (layer == NULL)
		return;

	if (layer->layer_surface != NULL)
		zwlr_layer_surface_v1_destroy(layer->layer_surface);
	if (layer->surface != NULL)
		wl_surface_destroy(layer->surface);

	wl_list_for_each_safe(buffer, tmp, &layer->buffers, link)
		nyx_buffer_destroy(buffer);

	layer->buffer = NULL;
	free(layer);
}

/**
 * nyx_layer_redraw() - Redraw a layer from the current image and settings.
 * @layer: layer to redraw. May be NULL, in which case nothing happens.
 *
 * Does nothing before the first configure, since the surface has no size
 * to draw to yet. Accepting NULL is what lets callers iterate over outputs
 * without checking whether each one has a surface.
 *
 * A render failure is reported and otherwise ignored: whatever was
 * committed before is still on screen and still valid, which beats
 * blanking the wallpaper. The layer stays dirty, so the next configure --
 * even one that repeats the current size -- tries again.
 */
void
nyx_layer_redraw(struct nyx_layer *layer)
{
	if (layer == NULL || !layer->configured)
		return;

	if (nyx_render_layer(layer->output->state, layer) < 0) {
		nyx_log("failed to render output %s",
		        output_label(layer->output));
		layer->dirty = 1;
		return;
	}

	layer->dirty = 0;
}
