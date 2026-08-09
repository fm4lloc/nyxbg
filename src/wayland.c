// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NyxBG - Connection, registry discovery and global interfaces.
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
 * Globals are bound at the lowest version that provides what NyxBG needs,
 * never at the highest the compositor offers. Binding higher would opt
 * this client into events it does not handle for no benefit, and would
 * make behaviour depend on the compositor's version rather than on this
 * program's own protocol description.
 *
 * The listener callbacks below take the proxy that delivered the event as
 * their second argument. NyxBG reaches everything it needs through the
 * data pointer instead, so those arguments are discarded explicitly rather
 * than by relaxing the project's warning set.
 */

#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include "output.h"
#include "util.h"
#include "wayland.h"

/* wl_surface v4 provides damage_buffer, which is the newest request the
 * renderer uses.  wl_output v4 provides the name event. */
#define NYX_COMPOSITOR_VERSION 4
#define NYX_OUTPUT_VERSION     4

/*
 * The layer shell is capped at 4 for the same reason as the other three:
 * a bound version is a promise to understand every event that version can
 * send, and this program implements exactly the two that exist up to 4.
 *
 * Binding at zwlr_layer_shell_v1_interface.version instead -- the version
 * of whatever XML the build machine happened to have -- would make the
 * same source produce binaries that speak different protocol versions on
 * different distributions, and a future event would be dispatched into the
 * NULL slot a designated initialiser leaves behind in the listener.
 *
 * Nothing here needs 5, which only adds set_exclusive_edge. The v3
 * destructor is still used where the compositor offers it; that is decided
 * at runtime by wl_proxy_get_version(), not by this constant.
 */
#define NYX_LAYER_SHELL_VERSION 4

/* Only version 1 of wl_shm is used: create_pool and the format event are
 * all this program needs. It goes through pick_version() like the rest so
 * that a registry advertising version 0 -- encodable, if absurd -- gets a
 * bind it can satisfy instead of a protocol error that kills the client at
 * the first roundtrip. */
#define NYX_SHM_VERSION 1

/**
 * pick_version() - Choose the version to bind a global at.
 * @advertised: version the compositor offers.
 * @wanted: highest version this program knows how to use.
 *
 * Return: The lower of the two, which is the highest version both sides
 * understand.
 */
static uint32_t
pick_version(uint32_t advertised, uint32_t wanted)
{
	return advertised < wanted ? advertised : wanted;
}

/**
 * release_compositor() - Drop the wl_compositor object.
 * @state: application state. Does nothing if it holds no compositor.
 */
static void
release_compositor(struct nyx_state *state)
{
	if (state->compositor == NULL)
		return;

	wl_compositor_destroy(state->compositor);
	state->compositor = NULL;
	state->compositor_name = 0;
}

/**
 * release_shm() - Drop the wl_shm object.
 * @state: application state. Does nothing if it holds no shm.
 *
 * Buffers and pools made from it are separate objects and outlive this
 * safely; they are torn down with the layer surfaces that own them.
 */
static void
release_shm(struct nyx_state *state)
{
	if (state->shm == NULL)
		return;

	wl_shm_destroy(state->shm);
	state->shm = NULL;
	state->shm_name = 0;
}

/**
 * release_layer_shell() - Drop the layer shell object.
 * @state: application state. Does nothing if it holds no layer shell.
 *
 * The explicit destructor request only exists from version 3 onwards.
 * Sending it to an older compositor would be a protocol error, so below
 * that the proxy is dropped locally instead.
 */
static void
release_layer_shell(struct nyx_state *state)
{
	if (state->layer_shell == NULL)
		return;

	if (wl_proxy_get_version((struct wl_proxy *)state->layer_shell) >=
	    ZWLR_LAYER_SHELL_V1_DESTROY_SINCE_VERSION)
		zwlr_layer_shell_v1_destroy(state->layer_shell);
	else
		wl_proxy_destroy((struct wl_proxy *)state->layer_shell);

	state->layer_shell = NULL;
	state->layer_shell_name = 0;
}

/**
 * already_bound() - Refuse a second bind of a singleton global.
 * @state: application state. Unused beyond the diagnostic.
 * @interface: interface name, for the diagnostic.
 * @bound: whether this program already holds an object for it.
 *
 * The three globals below are singletons in every real compositor, and the
 * code assumes it. A registry that advertises one of them twice -- which
 * the protocol encoding permits even though no conforming compositor does
 * it -- would otherwise overwrite the stored pointer, leaking the first
 * proxy along with the listener still attached to it, and leaving surfaces
 * created from one compositor object being given regions made from
 * another.
 *
 * Return: 1 if the caller must ignore this advertisement, 0 to proceed.
 */
static int
already_bound(const struct nyx_state *state, const char *interface, int bound)
{
	(void)state;

	if (!bound)
		return 0;

	nyx_log("ignoring a second %s global; one is already bound",
	        interface);
	return 1;
}

/* --------------------------------------------------------------------- */
/* wl_shm                                                                 */
/* --------------------------------------------------------------------- */

/**
 * shm_handle_format() - Record that the compositor supports a shm format.
 * @data: the struct nyx_state.
 * @shm: the wl_shm that sent the event. Unused.
 * @format: a &enum wl_shm_format value the compositor accepts.
 *
 * The renderer only ever emits XRGB8888, which every compositor is
 * required to support, so this exists to turn a violation of that
 * requirement into a warning at startup rather than a silent blank screen.
 */
static void
shm_handle_format(void *data, struct wl_shm *shm, uint32_t format)
{
	struct nyx_state *state = data;

	(void)shm;

	if (format == WL_SHM_FORMAT_XRGB8888)
		state->have_xrgb8888 = 1;
}

static const struct wl_shm_listener shm_listener = {
	shm_handle_format
};

/* --------------------------------------------------------------------- */
/* wl_registry                                                            */
/* --------------------------------------------------------------------- */

/**
 * registry_handle_global() - Bind a global NyxBG needs.
 * @data: the struct nyx_state.
 * @registry: the registry that sent the event.
 * @name: the global's registry name.
 * @interface: the global's interface name.
 * @version: the highest version the compositor offers.
 *
 * Anything not named here is ignored, which is most of what a compositor
 * advertises. Every global is bound at a constant compiled into this file,
 * never at whatever the generated protocol code happens to offer, so the
 * protocol version this program speaks is a property of the source and not
 * of the machine that built it.
 *
 * Outputs are handed straight to output.c; this function does not track
 * them itself.
 */
static void
registry_handle_global(void *data, struct wl_registry *registry,
                       uint32_t name, const char *interface, uint32_t version)
{
	struct nyx_state *state = data;

	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		if (already_bound(state, interface, state->compositor != NULL))
			return;
		state->compositor_version =
		    pick_version(version, NYX_COMPOSITOR_VERSION);
		state->compositor_name = name;
		state->compositor = wl_registry_bind(registry, name,
		                                     &wl_compositor_interface,
		                                     state->compositor_version);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		if (already_bound(state, interface, state->shm != NULL))
			return;
		state->shm_name = name;
		state->shm = wl_registry_bind(registry, name,
		    &wl_shm_interface, pick_version(version, NYX_SHM_VERSION));
		wl_shm_add_listener(state->shm, &shm_listener, state);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		if (already_bound(state, interface, state->layer_shell != NULL))
			return;
		state->layer_shell_name = name;
		state->layer_shell = wl_registry_bind(registry, name,
		    &zwlr_layer_shell_v1_interface,
		    pick_version(version, NYX_LAYER_SHELL_VERSION));
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		nyx_output_add(state, name,
		               pick_version(version, NYX_OUTPUT_VERSION));
	}
}

/**
 * registry_handle_global_remove() - React to a global going away.
 * @data: the struct nyx_state.
 * @registry: the registry that sent the event. Unused.
 * @name: registry name of the global that was removed.
 *
 * Only outputs are expected to come and go while the program runs; a
 * monitor being unplugged arrives here.
 *
 * The three singleton globals are checked too. A compositor is allowed to
 * withdraw them, and one that does leaves this program holding a proxy it
 * must not use again: further requests on it are silently discarded, so a
 * monitor hotplugged afterwards would get a surface that is never
 * configured and never drawn, with nothing said about why. Dropping the
 * pointer converts that into the diagnostics the render and create paths
 * already have for a missing global.
 *
 * The proxy is destroyed as well as forgotten. Surfaces, pools and buffers
 * already made from a factory global are independent protocol objects and
 * outlive it, so releasing the factory here is safe and is what the
 * registry's contract asks a client to do.
 */
static void
registry_handle_global_remove(void *data, struct wl_registry *registry,
                              uint32_t name)
{
	struct nyx_state *state = data;

	(void)registry;

	if (state->compositor != NULL && name == state->compositor_name) {
		nyx_log("the compositor withdrew wl_compositor");
		release_compositor(state);
	}
	if (state->shm != NULL && name == state->shm_name) {
		nyx_log("the compositor withdrew wl_shm; no further "
		        "wallpaper can be drawn");
		release_shm(state);
	}
	if (state->layer_shell != NULL && name == state->layer_shell_name) {
		nyx_log("the compositor withdrew the layer shell; new outputs "
		        "will not get a wallpaper");
		release_layer_shell(state);
	}

	nyx_output_remove(state, name);
}

static const struct wl_registry_listener registry_listener = {
	registry_handle_global,
	registry_handle_global_remove
};

/* --------------------------------------------------------------------- */
/* Lifecycle                                                              */
/* --------------------------------------------------------------------- */

/**
 * nyx_wayland_init() - Connect, bind the globals and get the first frame
 *                      on screen.
 * @state: application state, with its output list already initialised.
 *
 * Three round trips, each waiting for something the next one depends on.
 * The first discovers and binds the globals. The second delivers each
 * output's property burst, whose done event creates the layer surface --
 * which is why the layer shell has to be bound before it. The third lets
 * the compositor configure those surfaces, so the wallpaper is committed
 * before the event loop starts rather than one iteration into it.
 *
 * A compositor with no layer shell is the one hard failure: without it
 * there is no way to place a background surface, and no fall-back worth
 * having.
 *
 * Return: 0 on success. -1 on failure, with a diagnostic already printed;
 * the caller must still call nyx_wayland_finish() to release whatever was
 * bound before the failure.
 */
int
nyx_wayland_init(struct nyx_state *state)
{
	/* state->outputs is initialised by main.c, which owns the state, so
	 * that nyx_wayland_finish() is safe even if this function is never
	 * reached. */
	state->display = wl_display_connect(NULL);
	if (state->display == NULL) {
		nyx_log("cannot connect to a Wayland compositor "
		        "(is WAYLAND_DISPLAY set?)");
		return -1;
	}

	state->registry = wl_display_get_registry(state->display);
	if (state->registry == NULL) {
		nyx_log("cannot obtain the Wayland registry");
		return -1;
	}
	wl_registry_add_listener(state->registry, &registry_listener, state);

	/* First round trip: learn which globals exist and bind them. */
	if (wl_display_roundtrip(state->display) < 0) {
		nyx_log("Wayland connection failed during global discovery");
		return -1;
	}

	if (state->compositor == NULL) {
		nyx_log("compositor does not provide wl_compositor");
		return -1;
	}
	if (state->shm == NULL) {
		nyx_log("compositor does not provide wl_shm");
		return -1;
	}
	if (state->layer_shell == NULL) {
		nyx_log("compositor does not implement %s; NyxBG requires a "
		        "compositor with layer-shell support",
		        zwlr_layer_shell_v1_interface.name);
		return -1;
	}

	/*
	 * Second round trip: deliver the property burst for every output
	 * bound above.  Each output's done event creates its layer surface,
	 * which is why the layer shell has to be bound before this point.
	 */
	if (wl_display_roundtrip(state->display) < 0) {
		nyx_log("Wayland connection failed during output discovery");
		return -1;
	}

	/*
	 * Outputs at wl_output version 1 never send done, so their surfaces
	 * are not created by the handler above.  Everything they will ever
	 * tell us has arrived by now, so sweep them up here.
	 */
	nyx_output_create_surfaces(state);

	if (!state->have_xrgb8888)
		nyx_log("warning: compositor did not advertise XRGB8888; "
		        "rendering may fail");

	/* Third round trip: let the compositor configure the new surfaces so
	 * the wallpaper is on screen before the event loop starts. */
	if (wl_display_roundtrip(state->display) < 0) {
		nyx_log("Wayland connection failed during surface configuration");
		return -1;
	}

	if (wl_list_empty(&state->outputs))
		nyx_log("warning: no outputs are currently connected");

	return 0;
}

/**
 * nyx_wayland_finish() - Release every Wayland resource and disconnect.
 * @state: application state. Safe to pass one whose init failed part way,
 *         or one that never reached nyx_wayland_init() at all.
 *
 * Teardown runs in dependency order: outputs and their surfaces first,
 * then the globals those surfaces were created from, then the registry,
 * then the connection. The destructor requests are flushed before the
 * socket closes so the compositor sees an orderly teardown rather than a
 * hangup.
 *
 * The layer shell's explicit destructor request only exists from version 3
 * onwards. Sending it to an older compositor would be a protocol error, so
 * below that the proxy is dropped locally instead.
 *
 * Every pointer is cleared as it is released, so calling this twice is
 * harmless.
 */
void
nyx_wayland_finish(struct nyx_state *state)
{
	if (state->display == NULL)
		return;

	nyx_output_destroy_all(state);

	release_layer_shell(state);
	release_shm(state);
	release_compositor(state);

	if (state->registry != NULL) {
		wl_registry_destroy(state->registry);
		state->registry = NULL;
	}

	wl_display_flush(state->display);
	wl_display_disconnect(state->display);
	state->display = NULL;
}
