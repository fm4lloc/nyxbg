// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NyxBG - Entry point, command-line parsing, event loop and shutdown.
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
 * The event loop waits on exactly two descriptors: the Wayland connection
 * and the signal self-pipe. There is no timer and no redraw clock. Once
 * the wallpaper is committed the process sleeps in poll() until the
 * compositor or the user has something to say.
 *
 * The single struct nyx_state lives on this function's stack and is
 * released in one place, so every exit path -- a usage error, a failed
 * decode, a lost connection, a signal -- unwinds identically.
 */

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

#include "image.h"
#include "layer.h"
#include "output.h"
#include "scale.h"
#include "signal.h"
#include "util.h"
#include "wayland.h"

#ifndef NYXBG_VERSION
#define NYXBG_VERSION "0.0.0-unknown"
#endif

/* --------------------------------------------------------------------- */
/* Command line                                                           */
/* --------------------------------------------------------------------- */

/**
 * usage() - Print the option summary.
 * @out: stream to print to; stdout when the user asked, stderr when they
 *       made a mistake.
 *
 * Which stream this goes to is the difference between --help, whose output
 * is the point, and a usage error, whose output must not pollute a pipe.
 */
static void
usage(FILE *out)
{
	fputs(
	    "Usage: nyxbg [options] <image>\n"
	    "\n"
	    "Displays a static PNG or JPEG wallpaper on every output.\n"
	    "\n"
	    "Options:\n"
	    "  -m, --mode MODE     fill, fit, stretch or center (default: fill)\n"
	    "  -c, --color RRGGBB  colour drawn where the image does not reach\n"
	    "                      (default: 000000)\n"
	    "  -v, --verbose       print diagnostic information\n"
	    "  -h, --help          show this message and exit\n"
	    "  -V, --version       show version information and exit\n"
	    "\n"
	    "Signals:\n"
	    "  SIGHUP              reload the image from disk and redraw\n"
	    "  SIGINT, SIGTERM     shut down cleanly\n",
	    out);
}

/**
 * parse_arguments() - Interpret argv into the application state.
 * @argc: argument count as handed to main().
 * @argv: argument vector as handed to main().
 * @state: state to populate; its defaults must already be set.
 *
 * Parsed by hand rather than through getopt_long(), which is a GNU
 * extension: this keeps the program to POSIX plus the Wayland and codec
 * libraries. Both "--mode fill" and "--mode=fill" are accepted, and "--"
 * ends option processing so a file named like an option can still be
 * given.
 *
 * The image path points into @argv and is never copied, which is safe
 * because argv outlives every use of it.
 *
 * Return: 0 to continue, 1 to exit successfully because --help or
 * --version was handled, -1 on a usage error. Diagnostics have already
 * been printed on error.
 */
static int
parse_arguments(int argc, char **argv, struct nyx_state *state)
{
	const char *mode_name = NULL;
	const char *color_text = NULL;
	int options_ended = 0;
	int i;

	for (i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!options_ended && arg[0] == '-' && arg[1] != '\0') {
			if (strcmp(arg, "--") == 0) {
				options_ended = 1;
			} else if (strcmp(arg, "-h") == 0 ||
			           strcmp(arg, "--help") == 0) {
				usage(stdout);
				return 1;
			} else if (strcmp(arg, "-V") == 0 ||
			           strcmp(arg, "--version") == 0) {
				printf("nyxbg %s\n", NYXBG_VERSION);
				return 1;
			} else if (strcmp(arg, "-v") == 0 ||
			           strcmp(arg, "--verbose") == 0) {
				nyx_log_set_verbose(1);
			} else if (strcmp(arg, "-m") == 0 ||
			           strcmp(arg, "--mode") == 0) {
				if (++i >= argc) {
					nyx_log("%s requires an argument", arg);
					return -1;
				}
				mode_name = argv[i];
			} else if (strncmp(arg, "--mode=", 7) == 0) {
				mode_name = arg + 7;
			} else if (strcmp(arg, "-c") == 0 ||
			           strcmp(arg, "--color") == 0) {
				if (++i >= argc) {
					nyx_log("%s requires an argument", arg);
					return -1;
				}
				color_text = argv[i];
			} else if (strncmp(arg, "--color=", 8) == 0) {
				color_text = arg + 8;
			} else {
				nyx_log("unknown option: %s", arg);
				usage(stderr);
				return -1;
			}
			continue;
		}

		if (state->image_path != NULL) {
			nyx_log("only one image may be given");
			return -1;
		}
		state->image_path = arg;
	}

	if (state->image_path == NULL) {
		nyx_log("no image given");
		usage(stderr);
		return -1;
	}

	if (mode_name != NULL &&
	    nyx_scale_mode_parse(mode_name, &state->mode) < 0) {
		nyx_log("unknown scaling mode: %s "
		        "(expected fill, fit, stretch or center)", mode_name);
		return -1;
	}

	if (color_text != NULL &&
	    nyx_parse_color(color_text, &state->background) < 0) {
		nyx_log("invalid colour: %s (expected RRGGBB)", color_text);
		return -1;
	}

	return 0;
}

/* --------------------------------------------------------------------- */
/* Runtime events                                                         */
/* --------------------------------------------------------------------- */

/**
 * reload_image() - Re-read the wallpaper from disk and redraw everything.
 * @state: application state holding the path and the outputs.
 *
 * This is what SIGHUP does. The new image is decoded before the old one is
 * released, so a failed reload costs nothing: the wallpaper already on
 * screen is still valid and stays there.
 *
 * Outputs without a layer surface are handled by nyx_layer_redraw(), which
 * accepts NULL.
 */
static void
reload_image(struct nyx_state *state)
{
	struct nyx_output *output;
	struct nyx_image *image;

	nyx_debug("reloading %s", state->image_path);

	image = nyx_image_load(state->image_path);
	if (image == NULL) {
		nyx_log("reload failed, keeping the current wallpaper");
		return;
	}

	nyx_image_destroy(state->image);
	state->image = image;

	wl_list_for_each(output, &state->outputs, link)
		nyx_layer_redraw(output->layer);
}

/**
 * run() - The event loop.
 * @state: application state, connected and with its surfaces committed.
 * @signal_fd: readable end of the signal self-pipe.
 *
 * prepare_read()/read_events() rather than wl_display_dispatch(), because
 * it is the only way to block in this program's own poll() while still
 * guaranteeing no event is missed between checking for one and going to
 * sleep.
 *
 * The flush before poll() may report EAGAIN when the outgoing buffer is
 * full; POLLOUT is then added so the loop waits for room rather than
 * spinning. Every other flush error is fatal to the connection.
 *
 * The read must be cancelled on every path that does not reach
 * read_events(), or libwayland's reader count never returns to zero and
 * the next prepare_read() blocks forever.
 *
 * A signal that asks to terminate is honoured after any pending reload, so
 * a SIGHUP and a SIGTERM arriving together do not leave a half-applied
 * wallpaper.
 *
 * Return: 0 when the loop was left cleanly, -1 if the connection failed.
 */
static int
run(struct nyx_state *state, int signal_fd)
{
	struct pollfd fds[2];

	fds[0].fd = wl_display_get_fd(state->display);
	fds[0].events = POLLIN;
	fds[1].fd = signal_fd;
	fds[1].events = POLLIN;

	while (state->running) {
		int terminate = 0;
		int reload = 0;

		while (wl_display_prepare_read(state->display) != 0) {
			if (wl_display_dispatch_pending(state->display) < 0) {
				nyx_log("cannot dispatch Wayland events: %s",
				        strerror(errno));
				return -1;
			}
		}

		fds[0].events = POLLIN;
		if (wl_display_flush(state->display) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(state->display);
				nyx_log("cannot flush Wayland requests: %s",
				        strerror(errno));
				return -1;
			}
			/* The outgoing buffer is full; wait for room. */
			fds[0].events |= POLLOUT;
		}

		fds[0].revents = 0;
		fds[1].revents = 0;

		if (poll(fds, 2, -1) < 0) {
			/* Saved before cancel_read(), because a library call
			 * that succeeds is still permitted to set errno and
			 * the EINTR test below has to be about poll(). */
			int poll_errno = errno;

			wl_display_cancel_read(state->display);
			if (poll_errno == EINTR)
				continue;
			nyx_log("poll failed: %s", strerror(poll_errno));
			return -1;
		}

		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(state->display) < 0) {
				nyx_log("cannot read Wayland events: %s",
				        strerror(errno));
				return -1;
			}
		} else {
			wl_display_cancel_read(state->display);
		}

		if (wl_display_dispatch_pending(state->display) < 0) {
			nyx_log("cannot dispatch Wayland events: %s",
			        strerror(errno));
			return -1;
		}

		if (fds[1].revents & POLLIN) {
			if (nyx_signal_poll(&terminate, &reload) < 0)
				return -1;
			if (reload)
				reload_image(state);
			if (terminate) {
				nyx_debug("termination signal received");
				state->running = 0;
			}
		}

		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			/* Losing the compositor is a failure, not a normal
			 * exit: a supervisor has to be able to tell it apart
			 * from the SIGTERM it sent itself. */
			nyx_log("compositor closed the connection");
			return -1;
		}
	}

	return 0;
}

/* --------------------------------------------------------------------- */
/* Entry point                                                            */
/* --------------------------------------------------------------------- */

/**
 * main() - Parse, decode, connect, run, tear down.
 * @argc: argument count.
 * @argv: argument vector.
 *
 * The image is decoded before the compositor is contacted, so a bad file
 * fails without ever opening a connection. That also makes the binary
 * usable as a decoder test harness: with no WAYLAND_DISPLAY set it
 * decodes, reports, and exits.
 *
 * Every failure after that point jumps to one shutdown sequence, and each
 * of the three functions in it tolerates being called on something that
 * was never initialised.
 *
 * Return: EXIT_SUCCESS if the program ran and shut down cleanly, including
 * after --help or --version; EXIT_FAILURE otherwise.
 */
int
main(int argc, char **argv)
{
	struct nyx_state state;
	int status = EXIT_FAILURE;
	int signal_fd = -1;
	int parsed;

	memset(&state, 0, sizeof(state));
	state.mode = NYX_SCALE_DEFAULT;
	state.background = 0x000000;
	state.running = 1;
	wl_list_init(&state.outputs);

	parsed = parse_arguments(argc, argv, &state);
	if (parsed != 0)
		return parsed > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

	/* Decode before connecting: a bad image should fail without ever
	 * touching the compositor. */
	state.image = nyx_image_load(state.image_path);
	if (state.image == NULL)
		return EXIT_FAILURE;

	nyx_debug("mode %s, background %06x",
	          nyx_scale_mode_name(state.mode),
	          (unsigned int)state.background);

	signal_fd = nyx_signal_init();
	if (signal_fd < 0)
		goto out;

	if (nyx_wayland_init(&state) < 0)
		goto out;

	if (run(&state, signal_fd) == 0)
		status = EXIT_SUCCESS;

out:
	nyx_wayland_finish(&state);
	nyx_signal_finish();
	nyx_image_destroy(state.image);

	return status;
}
