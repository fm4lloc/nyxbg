// SPDX-License-Identifier: GPL-3.0-or-later
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
 * Every mode reduces to the same question: which rectangle of the source
 * maps onto which rectangle of the destination? Answering it here, with no
 * pixels in sight, keeps the renderer free of policy and makes the four
 * modes trivially testable in isolation.
 *
 * Aspect-ratio comparisons are done as cross-multiplications in 64-bit
 * integers rather than as floating-point divisions, so the branch taken is
 * exact for every representable input. The module allocates nothing and
 * keeps no state.
 */

#include <stddef.h>
#include <string.h>

#include "scale.h"
#include "util.h"

/* The one place a mode's spelling is written down. */
static const struct {
	const char *name;
	enum nyx_scale_mode mode;
} mode_names[] = {
	{ "fill",    NYX_SCALE_FILL    },
	{ "fit",     NYX_SCALE_FIT     },
	{ "stretch", NYX_SCALE_STRETCH },
	{ "center",  NYX_SCALE_CENTER  }
};

#define MODE_COUNT ((int)(sizeof(mode_names) / sizeof(mode_names[0])))

/**
 * nyx_scale_mode_parse() - Map a mode name to its enumerator.
 * @name: mode name as written on the command line. May be NULL.
 * @out: receives the matching mode. May be NULL.
 *
 * The comparison is exact and case-sensitive, so a misspelling is an error
 * rather than a silent fall-back to the default. @out is left untouched
 * when @name is not recognised.
 *
 * Return: 0 on success, -1 if @name is not a known mode.
 */
int
nyx_scale_mode_parse(const char *name, enum nyx_scale_mode *out)
{
	int i;

	if (name == NULL || out == NULL)
		return -1;

	for (i = 0; i < MODE_COUNT; i++) {
		if (strcmp(name, mode_names[i].name) == 0) {
			*out = mode_names[i].mode;
			return 0;
		}
	}

	return -1;
}

/**
 * nyx_scale_mode_name() - Return the canonical name of a mode.
 * @mode: mode to name.
 *
 * Used for diagnostics only.
 *
 * Return: The mode's name, or "unknown" if @mode is not a valid
 * enumerator. Never NULL, so the result can be passed straight to a "%s"
 * conversion.
 */
const char *
nyx_scale_mode_name(enum nyx_scale_mode mode)
{
	int i;

	for (i = 0; i < MODE_COUNT; i++) {
		if (mode_names[i].mode == mode)
			return mode_names[i].name;
	}

	return "unknown";
}

/**
 * image_is_wider() - Compare two aspect ratios exactly.
 * @iw: image width, positive.
 * @ih: image height, positive.
 * @ow: output width, positive.
 * @oh: output height, positive.
 *
 * Asks whether iw/ih > ow/oh, rewritten as iw*oh > ow*ih so that no
 * division and no floating point is involved. Both products are formed in
 * 64 bits, which no pair of int32_t operands can overflow, so the answer
 * is exact for every representable input rather than merely close.
 *
 * Return: Non-zero if the image is proportionally wider than the output.
 */
static int
image_is_wider(int32_t iw, int32_t ih, int32_t ow, int32_t oh)
{
	return (int64_t)iw * (int64_t)oh > (int64_t)ow * (int64_t)ih;
}

/**
 * clamp() - Confine a value to an inclusive range.
 * @value: value to confine.
 * @lo: lower bound.
 * @hi: upper bound. Must not be below @lo.
 *
 * Return: @value, or the nearer bound when it lies outside them.
 */
static int32_t
clamp(int32_t value, int32_t lo, int32_t hi)
{
	if (value < lo)
		return lo;
	if (value > hi)
		return hi;

	return value;
}

/**
 * nyx_scale_compute() - Work out which part of the image goes where.
 * @mode: scaling mode to apply.
 * @iw: image width, in pixels. Must be positive.
 * @ih: image height, in pixels. Must be positive.
 * @ow: output width, in pixels. Must be positive.
 * @oh: output height, in pixels. Must be positive.
 * @g: receives the source and destination rectangles. May be NULL, which
 *     is rejected.
 *
 * Each mode picks the rectangles differently, but all four then centre
 * them the same way, which is why the offsets are computed once at the
 * end rather than four times.
 *
 * Every intermediate ratio goes through nyx_scale_ratio(), which is
 * overflow-safe, and is then clamped to the surface it indexes, so a
 * rounding step cannot produce a rectangle that reaches outside its
 * surface.
 *
 * Return: 0 on success, with @g fully populated. -1 if @g is NULL, if any
 * dimension is not positive, or if @mode is not a valid enumerator. On a
 * rejected argument @g is not written to at all; on a rejected @mode it
 * has already been zeroed. Either way its contents are not meaningful and
 * the caller must not use them.
 */
int
nyx_scale_compute(enum nyx_scale_mode mode,
                  int32_t iw, int32_t ih,
                  int32_t ow, int32_t oh,
                  struct nyx_geometry *g)
{
	if (g == NULL || iw <= 0 || ih <= 0 || ow <= 0 || oh <= 0)
		return -1;

	memset(g, 0, sizeof(*g));

	switch (mode) {
	case NYX_SCALE_STRETCH:
		g->src_w = iw;
		g->src_h = ih;
		g->dst_w = ow;
		g->dst_h = oh;
		break;

	case NYX_SCALE_FILL:
		/* Largest sub-rectangle of the image having the output's
		 * aspect ratio.  The image is cropped, never letterboxed. */
		if (image_is_wider(iw, ih, ow, oh)) {
			g->src_h = ih;
			g->src_w = clamp(nyx_scale_ratio(ih, ow, oh), 1, iw);
		} else {
			g->src_w = iw;
			g->src_h = clamp(nyx_scale_ratio(iw, oh, ow), 1, ih);
		}
		g->dst_w = ow;
		g->dst_h = oh;
		break;

	case NYX_SCALE_FIT:
		/* Largest sub-rectangle of the output having the image's
		 * aspect ratio.  The image is letterboxed, never cropped. */
		g->src_w = iw;
		g->src_h = ih;
		if (image_is_wider(iw, ih, ow, oh)) {
			g->dst_w = ow;
			g->dst_h = clamp(nyx_scale_ratio(ow, ih, iw), 1, oh);
		} else {
			g->dst_h = oh;
			g->dst_w = clamp(nyx_scale_ratio(oh, iw, ih), 1, ow);
		}
		break;

	case NYX_SCALE_CENTER:
		/* One image pixel per output pixel.  Whichever axis does not
		 * fit is cropped; whichever axis is smaller is letterboxed. */
		g->src_w = g->dst_w = (iw < ow) ? iw : ow;
		g->src_h = g->dst_h = (ih < oh) ? ih : oh;
		break;

	default:
		return -1;
	}

	g->src_x = (iw - g->src_w) / 2;
	g->src_y = (ih - g->src_h) / 2;
	g->dst_x = (ow - g->dst_w) / 2;
	g->dst_y = (oh - g->dst_h) / 2;

	return 0;
}
