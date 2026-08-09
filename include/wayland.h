/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NyxBG - Wayland connection, registry and global interfaces.
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
 * This header also declares struct nyx_state, the single application
 * context. NyxBG is a Wayland client and nothing else, so the connection
 * is the natural root of ownership: every other object in the program is
 * reachable from here, and lifetimes are explicit rather than implied by
 * global variables.
 */
#ifndef NYXBG_WAYLAND_H
#define NYXBG_WAYLAND_H

#include <stdint.h>
#include <wayland-client.h>

#include "scale.h"

struct nyx_image;
struct zwlr_layer_shell_v1;

/**
 * struct nyx_state - The whole application context.
 * @image_path: path given on the command line; points into argv and is
 *              never freed.
 * @mode: scaling mode chosen on the command line.
 * @background: letterbox colour as 0x00RRGGBB.
 * @image: the decoded wallpaper. Owned here; replaced on SIGHUP.
 * @display: the compositor connection. Owned here.
 * @registry: the registry object. Owned here.
 * @compositor: the wl_compositor global. Owned here.
 * @shm: the wl_shm global. Owned here.
 * @layer_shell: the zwlr_layer_shell_v1 global. Owned here.
 * @compositor_version: version wl_compositor was bound at. Recorded for
 *                      diagnostics only. What decides whether a versioned
 *                      surface request may be sent is
 *                      wl_proxy_get_version() on the surface itself, asked
 *                      at the point of use in render.c, because that is
 *                      the object the request goes to.
 * @outputs: list of struct nyx_output, linked through its @link member.
 * @running: cleared to leave the event loop.
 * @have_xrgb8888: set once wl_shm has advertised the only format the
 *                 renderer emits.
 *
 * One instance lives on main()'s stack for the lifetime of the process.
 * The first three fields are written once during argument parsing and are
 * read-only afterwards.
 */
struct nyx_state {
	/* ---- Configuration.  Written once by main.c, read-only after. ---- */
	const char *image_path;
	enum nyx_scale_mode mode;
	uint32_t background;

	/* ---- Decoded source image.  Owned here. ---- */
	struct nyx_image *image;

	/* ---- Wayland globals.  Owned here. ---- */
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct zwlr_layer_shell_v1 *layer_shell;

	uint32_t compositor_version;

	/* Registry names of the three singleton globals, so that a
	 * global_remove naming one of them can be acted on rather than
	 * silently leaving a dead proxy in use. Zero means unbound. */
	uint32_t compositor_name;
	uint32_t shm_name;
	uint32_t layer_shell_name;

	/* ---- Outputs. ---- */
	struct wl_list outputs;

	/* ---- Event loop state. ---- */
	int running;
	int have_xrgb8888;
};

int nyx_wayland_init(struct nyx_state *state);

void nyx_wayland_finish(struct nyx_state *state);

#endif /* NYXBG_WAYLAND_H */
