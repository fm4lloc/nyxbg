// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NyxBG - Output discovery, hotplug, resolution, scale and transform.
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
 * The compositor sends an output's properties as a burst terminated by a
 * done event. Acting only on done means the layer surface is created once,
 * with a coherent view of the output, rather than once per property. The
 * same handler covers later changes: a resolution or scale change arrives
 * as another burst and ends with another done.
 *
 * The listener callbacks reach their output through the data pointer, so
 * the wl_output argument every event carries is discarded explicitly.
 */

#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "layer.h"
#include "output.h"
#include "util.h"
#include "wayland.h"

/**
 * output_handle_geometry() - Record the output's physical placement.
 * @data: the struct nyx_output.
 * @wl_output: the output that sent the event. Unused.
 * @x: position of the output in the compositor space. Unused.
 * @y: position of the output in the compositor space. Unused.
 * @physical_width: screen width in millimetres. Unused.
 * @physical_height: screen height in millimetres. Unused.
 * @subpixel: subpixel layout. Unused; NyxBG does no text rendering.
 * @make: manufacturer string. Unused.
 * @model: model string. Unused.
 * @transform: the output's rotation or flip.
 *
 * Only the transform is kept, and only for diagnostics: the compositor
 * applies the rotation itself, so the buffer is always drawn unrotated.
 */
static void
output_handle_geometry(void *data, struct wl_output *wl_output,
                       int32_t x, int32_t y,
                       int32_t physical_width, int32_t physical_height,
                       int32_t subpixel, const char *make, const char *model,
                       int32_t transform)
{
	struct nyx_output *output = data;

	(void)wl_output;
	(void)x;
	(void)y;
	(void)physical_width;
	(void)physical_height;
	(void)subpixel;
	(void)make;
	(void)model;

	output->transform = transform;
}

/**
 * output_handle_mode() - Record the output's current resolution.
 * @data: the struct nyx_output.
 * @wl_output: the output that sent the event. Unused.
 * @flags: mode flags; a &enum wl_output_mode bitmask.
 * @width: mode width in physical pixels.
 * @height: mode height in physical pixels.
 * @refresh: refresh rate in mHz. Unused; NyxBG does not animate.
 *
 * A compositor advertises every mode the monitor supports and marks one
 * current. Only that one is of any interest here.
 *
 * The size kept here is for diagnostics. The authoritative surface size is
 * the one the layer surface's configure event reports.
 */
static void
output_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags,
                   int32_t width, int32_t height, int32_t refresh)
{
	struct nyx_output *output = data;

	(void)wl_output;
	(void)refresh;

	if ((flags & (uint32_t)WL_OUTPUT_MODE_CURRENT) == 0)
		return;

	output->width = width;
	output->height = height;
}

/**
 * output_handle_scale() - Record the output's integer buffer scale.
 * @data: the struct nyx_output.
 * @wl_output: the output that sent the event. Unused.
 * @factor: buffer scale factor.
 *
 * The compositor is only semi-trusted, and this value multiplies the
 * surface size to give the buffer size. Bounding it here, where it enters
 * the program, means the renderer's multiplication is bounded by
 * construction rather than by a check that could be forgotten.
 *
 * A non-positive factor is meaningless and is dropped; an implausibly
 * large one is clamped rather than dropped, so a compositor that
 * misreports still gets a wallpaper.
 */
static void
output_handle_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
	struct nyx_output *output = data;

	(void)wl_output;

	if (factor < 1) {
		nyx_log("ignoring non-positive output scale %d", factor);
		return;
	}
	if (factor > NYX_MAX_OUTPUT_SCALE) {
		nyx_log("clamping implausible output scale %d to %d",
		        factor, NYX_MAX_OUTPUT_SCALE);
		factor = NYX_MAX_OUTPUT_SCALE;
	}

	output->scale = factor;
}

/**
 * output_handle_name() - Record the output's connector name.
 * @data: the struct nyx_output.
 * @wl_output: the output that sent the event. Unused.
 * @name: connector name such as "HDMI-A-1".
 *
 * Diagnostics only, and only available from wl_output version 4. The
 * string is copied because the one passed in belongs to libwayland and is
 * valid only for the duration of the call.
 *
 * Any previous name is released first: the compositor is permitted to
 * re-send the property burst.
 */
static void
output_handle_name(void *data, struct wl_output *wl_output, const char *name)
{
	struct nyx_output *output = data;

	(void)wl_output;

	free(output->name);
	output->name = nyx_strdup(name);
}

/**
 * output_handle_description() - Ignore the output's human-readable
 *                               description.
 * @data: the struct nyx_output. Unused.
 * @wl_output: the output that sent the event. Unused.
 * @description: prose such as "Dell Inc. 27 inch monitor". Unused.
 *
 * The event has to be handled -- a NULL listener entry would be a null
 * call -- but NyxBG has no use for the text. The connector name from
 * output_handle_name() is what diagnostics use.
 */
static void
output_handle_description(void *data, struct wl_output *wl_output,
                          const char *description)
{
	(void)data;
	(void)wl_output;
	(void)description;
}

/**
 * output_handle_done() - Act on a completed property burst.
 * @data: the struct nyx_output.
 * @wl_output: the output that sent the event. Unused.
 *
 * Everything the compositor had to say about this output has now been
 * said, so this is the first point at which acting on it is safe.
 *
 * On the first burst the layer surface is created, unless the layer shell
 * has not been bound yet -- in which case nyx_output_create_surfaces()
 * picks the output up once it has.
 *
 * On a later burst the surface already exists. Only a scale change needs
 * anything done here, because it changes the buffer's pixel size; a
 * resolution change arrives separately as a layer surface configure and is
 * handled there.
 */
static void
output_handle_done(void *data, struct wl_output *wl_output)
{
	struct nyx_output *output = data;

	(void)wl_output;

	output->done_seen = 1;

	nyx_debug("output %s: %dx%d, scale %d, transform %d",
	          output->name != NULL ? output->name : "?",
	          output->width, output->height, output->scale,
	          output->transform);

	if (output->layer == NULL) {
		/* A surface the compositor closed stays closed. Recreating it
		 * here would turn one closed event into an unbounded
		 * create-and-destroy loop driven by repeated done events. */
		if (output->state->layer_shell != NULL && !output->layer_closed)
			output->layer = nyx_layer_create(output);
		return;
	}

	if (output->layer->scale != output->scale)
		nyx_layer_redraw(output->layer);
}

static const struct wl_output_listener output_listener = {
	output_handle_geometry,
	output_handle_mode,
	output_handle_done,
	output_handle_scale,
	output_handle_name,
	output_handle_description
};

/**
 * nyx_output_add() - Start tracking a newly advertised output.
 * @state: application state to attach the output to.
 * @global_name: the output's registry name.
 * @version: version to bind the wl_output at.
 *
 * The scale is seeded to 1 so that an output whose compositor never sends
 * a scale event still renders at a sane size.
 *
 * wl_output gained the done event in version 2. Below that the property
 * burst has no terminator, so such an output is marked complete
 * immediately and nyx_output_create_surfaces() creates its surface once
 * the initial round trip has delivered everything.
 *
 * A bind failure is reported and the output is dropped; the rest of the
 * outputs are unaffected.
 */
void
nyx_output_add(struct nyx_state *state, uint32_t global_name, uint32_t version)
{
	struct nyx_output *output;

	output = nyx_alloc(1, sizeof(*output));
	output->state = state;
	output->global_name = global_name;
	output->scale = 1;

	output->wl_output = wl_registry_bind(state->registry, global_name,
	                                     &wl_output_interface, version);
	if (output->wl_output == NULL) {
		nyx_log("cannot bind output %u", global_name);
		free(output);
		return;
	}

	if (version < 2)
		output->done_seen = 1;

	wl_output_add_listener(output->wl_output, &output_listener, output);
	wl_list_insert(&state->outputs, &output->link);

	nyx_debug("output %u added (interface version %u)", global_name, version);
}

/**
 * nyx_output_remove() - Stop tracking an output that has gone away.
 * @state: application state holding the output list.
 * @global_name: registry name from the global_remove event.
 *
 * Registry names are not namespaced per interface, so a name that belongs
 * to some other kind of global will reach here too. Those simply match
 * nothing and are ignored.
 *
 * Every match is removed, not just the first. A conforming compositor
 * never advertises one name twice, so there is normally exactly one; a
 * compositor that does it anyway would otherwise leave the duplicate
 * holding a proxy bound to a global that no longer exists.
 */
void
nyx_output_remove(struct nyx_state *state, uint32_t global_name)
{
	struct nyx_output *output, *tmp;

	wl_list_for_each_safe(output, tmp, &state->outputs, link) {
		if (output->global_name != global_name)
			continue;

		nyx_debug("output %s removed",
		          output->name != NULL ? output->name : "?");
		nyx_output_destroy(output);
	}
}

/**
 * nyx_output_destroy() - Release one output and everything it owns.
 * @output: output to release. May be NULL, in which case nothing happens.
 *
 * The layer surface is destroyed before the wl_output it was created
 * against, because the compositor associates the two.
 *
 * wl_output.release, which tells the compositor the object is gone, only
 * exists from version 3; below that the proxy is destroyed locally
 * instead.
 */
void
nyx_output_destroy(struct nyx_output *output)
{
	if (output == NULL)
		return;

	wl_list_remove(&output->link);

	nyx_layer_destroy(output->layer);

	if (output->wl_output != NULL) {
		if (wl_proxy_get_version((struct wl_proxy *)output->wl_output) >=
		    WL_OUTPUT_RELEASE_SINCE_VERSION)
			wl_output_release(output->wl_output);
		else
			wl_output_destroy(output->wl_output);
	}

	free(output->name);
	free(output);
}

/**
 * nyx_output_destroy_all() - Release every tracked output.
 * @state: application state holding the output list.
 *
 * Safe to call on a state whose list was never initialised, which is what
 * the NULL check on the head covers: shutdown runs even when startup
 * failed before that point.
 */
void
nyx_output_destroy_all(struct nyx_state *state)
{
	struct nyx_output *output, *tmp;

	if (state->outputs.next == NULL)
		return;

	wl_list_for_each_safe(output, tmp, &state->outputs, link)
		nyx_output_destroy(output);
}

/**
 * nyx_output_create_surfaces() - Give a surface to every output still
 *                                without one.
 * @state: application state holding the output list.
 *
 * Two cases need this sweep. An output announced before the layer shell
 * was bound could not have a surface created in its done handler, and an
 * output at wl_output version 1 never sends done at all.
 *
 * Outputs that already have a surface, and outputs whose properties are
 * still arriving, are left alone.
 */
void
nyx_output_create_surfaces(struct nyx_state *state)
{
	struct nyx_output *output;

	if (state->layer_shell == NULL)
		return;

	wl_list_for_each(output, &state->outputs, link) {
		if (output->done_seen && output->layer == NULL &&
		    !output->layer_closed)
			output->layer = nyx_layer_create(output);
	}
}
