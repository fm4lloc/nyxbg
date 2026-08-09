// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NyxBG - Shared-memory buffers, resampling, attach and commit.
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
 * Resampling uses a separable triangle filter whose support is widened
 * when downscaling. At a 1:1 or magnifying ratio the support is one source
 * pixel and the filter degenerates to bilinear interpolation; when
 * minifying by a factor of n the support becomes n source pixels, so every
 * source pixel inside the footprint contributes. That is what stops a
 * 6000-pixel-wide photo from aliasing into a 1920-pixel-wide output, which
 * is the failure mode a naive bilinear or nearest sampler exhibits.
 *
 * Filtering is done on premultiplied alpha -- interpolating straight alpha
 * bleeds colour out of transparent pixels -- and the result is composited
 * over the configured background before being written out as opaque
 * XRGB8888.
 *
 * Pixels are written byte by byte rather than as a native uint32_t. The
 * wl_shm XRGB8888 format is defined as a little-endian 32-bit quantity, so
 * an explicit B,G,R,X byte order is correct on any host.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "image.h"
#include "layer.h"
#include "output.h"
#include "render.h"
#include "scale.h"
#include "util.h"
#include "wayland.h"

#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__linux__) && defined(SYS_memfd_create)
#define NYX_USE_MEMFD 1
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#endif

/* NYX_MAX_BUFFER_DIMENSION lives in render.h; it bounds both axes here and
 * the surface sizes layer.c accepts from the compositor. */

/* --------------------------------------------------------------------- */
/* Shared memory                                                          */
/* --------------------------------------------------------------------- */

/**
 * create_shm_file() - Obtain an anonymous file to share with the
 *                     compositor.
 * @size: length to give the file, in bytes.
 *
 * memfd_create() is preferred where it exists: the file has no name, so
 * there is no window in which another process could open it, and no
 * cleanup to get wrong.
 *
 * The fall-back path is POSIX shm_open(), whose object does have a name.
 * It is unlinked immediately after creation, so the name exists only
 * between those two calls, and O_EXCL makes a collision an error rather
 * than an adoption of somebody else's object. A handful of attempts is
 * enough: the name carries the process id and a counter, so a collision
 * means a stale object from a previous run with the same pid.
 *
 * The syscall is invoked directly rather than through glibc's wrapper so
 * the code builds against a libc too old to declare memfd_create().
 *
 * Return: An open descriptor of @size bytes, or -1 with a diagnostic
 * already printed.
 */
static int
create_shm_file(size_t size)
{
	int fd = -1;

#ifdef NYX_USE_MEMFD
	long raw = syscall(SYS_memfd_create, "nyxbg", MFD_CLOEXEC);

	fd = (raw < 0) ? -1 : (int)raw;
#endif

	if (fd < 0) {
		static unsigned int counter;
		char name[64];
		int attempt;

		for (attempt = 0; attempt < 16; attempt++) {
			snprintf(name, sizeof(name), "/nyxbg-%lu-%u",
			         (unsigned long)getpid(), counter++);
			fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (fd >= 0) {
				shm_unlink(name);
				break;
			}
			if (errno != EEXIST) {
				nyx_log("cannot create shared memory: %s",
				        strerror(errno));
				return -1;
			}
		}
		if (fd < 0) {
			nyx_log("cannot find an unused shared memory name");
			return -1;
		}
	}

	if (ftruncate(fd, (off_t)size) < 0) {
		nyx_log("cannot size shared memory to %lu bytes: %s",
		        (unsigned long)size, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

/* --------------------------------------------------------------------- */
/* Buffers                                                                */
/* --------------------------------------------------------------------- */

static void buffer_handle_release(void *data, struct wl_buffer *wl_buffer);

static const struct wl_buffer_listener buffer_listener = {
	buffer_handle_release
};

/**
 * buffer_handle_release() - Take a buffer back from the compositor.
 * @data: the struct nyx_buffer.
 * @wl_buffer: the protocol object that sent the event. Unused.
 *
 * The compositor has finished reading the buffer, so it may be drawn into
 * again or destroyed. A buffer marked doomed was replaced while the
 * compositor still held it and was waiting for exactly this event.
 */
static void
buffer_handle_release(void *data, struct wl_buffer *wl_buffer)
{
	struct nyx_buffer *buffer = data;
	struct nyx_layer *layer;

	(void)wl_buffer;

	buffer->busy = 0;
	layer = buffer->layer;

	/* A released current buffer is no longer usable as the tracked current
	 * content. Clearing the pointer before destroying the proxy also makes
	 * the object safe to reclaim below. */
	if (layer != NULL && layer->buffer == buffer)
		layer->buffer = NULL;

	nyx_buffer_destroy(buffer);

	/* Any release can free one slot in the bounded buffer list. If a
	 * configure or reload left the layer dirty, retry immediately rather
	 * than waiting for another configure that may never arrive. */
	if (layer != NULL && layer->dirty)
		nyx_layer_redraw(layer);
}

/**
 * nyx_buffer_destroy() - Release a buffer and its mapping immediately.
 * @buffer: buffer to release. May be NULL, in which case nothing happens.
 *
 * Destroys regardless of the busy flag, so callers must be sure the
 * compositor can no longer reach the buffer -- either because it has been
 * released, or because the surface it was attached to is already gone.
 */
void
nyx_buffer_destroy(struct nyx_buffer *buffer)
{
	if (buffer == NULL)
		return;

	if (buffer->link.next != NULL)
		wl_list_remove(&buffer->link);

	if (buffer->wl_buffer != NULL)
		wl_buffer_destroy(buffer->wl_buffer);
	if (buffer->data != NULL && buffer->data != MAP_FAILED)
		munmap(buffer->data, buffer->size);

	free(buffer);
}

/**
 * buffer_create() - Allocate a shared-memory buffer of a given size.
 * @shm: the wl_shm global to create the pool from.
 * @width: buffer width in pixels.
 * @height: buffer height in pixels.
 *
 * The per-axis bound alone does not bound the product: 32767 x 16385 is
 * already over two gigabytes. wl_shm_create_pool takes the size as an
 * int32_t, so anything above INT32_MAX would reach the compositor as a
 * negative number; that is checked separately.
 *
 * The pool is destroyed as soon as the buffer has been created from it.
 * The mapping and the descriptor outlive it: a pool is only a handle for
 * carving buffers out of the memory, not the memory itself.
 *
 * Return: The new buffer, not yet on any list, or NULL with a diagnostic
 * already printed.
 */
static struct nyx_buffer *
buffer_create(struct wl_shm *shm, int32_t width, int32_t height)
{
	struct nyx_buffer *buffer;
	struct wl_shm_pool *pool;
	size_t stride, size;
	void *data;
	int fd;

	if (width <= 0 || height <= 0 ||
	    width > NYX_MAX_BUFFER_DIMENSION || height > NYX_MAX_BUFFER_DIMENSION) {
		nyx_log("refusing to allocate a %dx%d buffer", width, height);
		return NULL;
	}

	stride = (size_t)width * 4;
	size = stride * (size_t)height;

	if (size > (size_t)INT32_MAX) {
		nyx_log("refusing a %dx%d buffer: %lu bytes exceeds the "
		        "maximum shared memory pool size",
		        width, height, (unsigned long)size);
		return NULL;
	}

	fd = create_shm_file(size);
	if (fd < 0)
		return NULL;

	data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		nyx_log("cannot map shared memory: %s", strerror(errno));
		close(fd);
		return NULL;
	}

	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	close(fd);
	if (pool == NULL) {
		nyx_log("cannot create shared memory pool");
		munmap(data, size);
		return NULL;
	}

	buffer = nyx_alloc(1, sizeof(*buffer));
	buffer->layer = NULL;
	buffer->data = data;
	buffer->size = size;
	buffer->width = width;
	buffer->height = height;
	buffer->stride = (int32_t)stride;

	buffer->wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
	                                              (int32_t)stride,
	                                              WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);

	if (buffer->wl_buffer == NULL) {
		nyx_log("cannot create wl_buffer");
		munmap(data, size);
		free(buffer);
		return NULL;
	}

	wl_buffer_add_listener(buffer->wl_buffer, &buffer_listener, buffer);

	return buffer;
}

/* --------------------------------------------------------------------- */
/* Separable filter                                                       */
/* --------------------------------------------------------------------- */

/**
 * struct filter_axis - Precomputed resampling weights for one axis.
 * @dst_len: number of destination samples.
 * @max_taps: capacity of each weight row; the stride of @weights.
 * @start: @dst_len entries, the first source index each output reads.
 * @count: @dst_len entries, how many source samples each output reads.
 * @weights: @dst_len * @max_taps entries, each row normalised to sum to 1.
 *
 * Building the weights once per axis rather than per pixel is what makes
 * the two-pass resample affordable: a destination row reuses the same
 * horizontal weights for every source row.
 */
struct filter_axis {
	int32_t dst_len;
	int32_t max_taps;
	int32_t *start;
	int32_t *count;
	float *weights;
};

/**
 * filter_axis_finish() - Release an axis and leave it re-initialised.
 * @axis: axis to release. Must have been zeroed or built.
 *
 * Zeroing afterwards makes the function idempotent, which is what lets the
 * error path in blit_resampled() call it on an axis that was never built.
 */
static void
filter_axis_finish(struct filter_axis *axis)
{
	free(axis->start);
	free(axis->count);
	free(axis->weights);
	memset(axis, 0, sizeof(*axis));
}

/**
 * filter_axis_build() - Compute the resampling weights for one axis.
 * @axis: axis to fill in.
 * @src_origin: first source index of the cropped region.
 * @src_len: length of the cropped region, in source samples.
 * @dst_len: number of destination samples to produce.
 *
 * The support widens with the reduction factor: at a ratio of n source
 * samples per destination sample the triangle spans n samples either side,
 * so every source sample inside the footprint contributes and nothing is
 * skipped. At a ratio of 1 or below the support is one sample and the
 * filter degenerates to linear interpolation.
 *
 * Taps are clamped to the source interval, so sampling never reaches
 * outside the cropped region -- which is what makes the crop in "fill"
 * safe without a border. A window that collapses entirely, possible only
 * at the very edge, falls back to the single nearest sample.
 *
 * Each row is normalised to sum to 1, so the filter preserves brightness
 * regardless of how many taps survived clamping.
 *
 * Return: 0 on success, -1 if either length is not positive. On failure
 * the axis is left zeroed and safe to pass to filter_axis_finish().
 */
static int
filter_axis_build(struct filter_axis *axis, int32_t src_origin,
                  int32_t src_len, int32_t dst_len)
{
	double ratio, support, span;
	int32_t i;
	int32_t src_lo = src_origin;
	int32_t src_hi = src_origin + src_len - 1;

	memset(axis, 0, sizeof(*axis));

	if (src_len <= 0 || dst_len <= 0)
		return -1;

	ratio = (double)src_len / (double)dst_len;
	support = (ratio > 1.0) ? ratio : 1.0;

	/* The tap window spans 2 * support source pixels, so this bound is
	 * never exceeded; the extra slack covers the ceil() boundary. */
	span = ceil(support * 2.0);
	axis->max_taps = (int32_t)span + 2;
	axis->dst_len = dst_len;
	axis->start = nyx_alloc((size_t)dst_len, sizeof(int32_t));
	axis->count = nyx_alloc((size_t)dst_len, sizeof(int32_t));
	axis->weights = nyx_alloc((size_t)dst_len * (size_t)axis->max_taps,
	                          sizeof(float));

	for (i = 0; i < dst_len; i++) {
		double center = ((double)i + 0.5) * ratio - 0.5 + (double)src_origin;
		double first = ceil(center - support);
		double last = floor(center + support);
		double sum = 0.0;
		int32_t lo = (int32_t)first;
		int32_t hi = (int32_t)last;
		int32_t j, n;
		float *w = axis->weights + (size_t)i * (size_t)axis->max_taps;

		if (lo < src_lo)
			lo = src_lo;
		if (hi > src_hi)
			hi = src_hi;
		if (hi < lo) {
			double nearest = floor(center + 0.5);

			lo = hi = (int32_t)nearest;
			if (lo < src_lo)
				lo = src_lo;
			if (lo > src_hi)
				lo = src_hi;
			hi = lo;
		}

		n = hi - lo + 1;
		if (n > axis->max_taps)
			n = axis->max_taps;

		for (j = 0; j < n; j++) {
			double distance = fabs((double)(lo + j) - center) / support;
			double weight = 1.0 - distance;

			if (weight < 0.0)
				weight = 0.0;
			w[j] = (float)weight;
			sum += weight;
		}

		if (sum <= 0.0) {
			/* Can only happen if the window collapsed onto a single
			 * clamped pixel exactly one support away. */
			for (j = 0; j < n; j++)
				w[j] = 0.0f;
			w[0] = 1.0f;
		} else {
			for (j = 0; j < n; j++)
				w[j] = (float)((double)w[j] / sum);
		}

		axis->start[i] = lo;
		axis->count[i] = n;
	}

	return 0;
}

/* --------------------------------------------------------------------- */
/* Drawing                                                                */
/* --------------------------------------------------------------------- */

/**
 * clamp_byte() - Round a channel value and confine it to 0..255.
 * @value: channel value, nominally already in range.
 *
 * Accumulated filter weights can leave a value a fraction outside the
 * range even though each tap was in it, so the clamp is not optional.
 *
 * Return: The rounded, clamped value.
 */
static unsigned char
clamp_byte(float value)
{
	if (value <= 0.0f)
		return 0;
	if (value >= 255.0f)
		return 255;

	return (unsigned char)(value + 0.5f);
}

/**
 * fill_background() - Paint the whole buffer with one colour.
 * @buffer: buffer to paint.
 * @color: colour as 0x00RRGGBB.
 *
 * The first row is written pixel by pixel and every other row is copied
 * from it, so the per-pixel work happens once per buffer rather than once
 * per pixel. The X byte is set to 0xff although XRGB8888 ignores it,
 * because leaving it at whatever the mapping held is untidy.
 */
static void
fill_background(struct nyx_buffer *buffer, uint32_t color)
{
	unsigned char *base = buffer->data;
	unsigned char pattern[4];
	int32_t x, y;

	pattern[0] = (unsigned char)(color & 0xff);          /* B */
	pattern[1] = (unsigned char)((color >> 8) & 0xff);   /* G */
	pattern[2] = (unsigned char)((color >> 16) & 0xff);  /* R */
	pattern[3] = 0xff;                                   /* X, ignored */

	for (x = 0; x < buffer->width; x++)
		memcpy(base + (size_t)x * 4, pattern, 4);

	for (y = 1; y < buffer->height; y++)
		memcpy(base + (size_t)y * (size_t)buffer->stride, base,
		       (size_t)buffer->stride);
}

/**
 * compose_pixel() - Write one destination pixel over the background.
 * @dst: four bytes to write, in B,G,R,X order.
 * @rgba: source pixel with premultiplied colour channels and straight
 *        alpha, each 0..255.
 * @bg: background colour as R,G,B floats, each 0..255.
 *
 * Because the colour channels are already multiplied by alpha, compositing
 * is one multiply-add per channel: the source contributes what it has and
 * the background fills in the remaining transparency.
 *
 * The output is opaque XRGB8888 in explicit little-endian byte order, so
 * the result does not depend on the host's endianness.
 */
static void
compose_pixel(unsigned char *dst, const float rgba[4], const float bg[3])
{
	float transparency = 1.0f - rgba[3] / 255.0f;

	if (transparency < 0.0f)
		transparency = 0.0f;

	dst[0] = clamp_byte(rgba[2] + bg[2] * transparency);  /* B */
	dst[1] = clamp_byte(rgba[1] + bg[1] * transparency);  /* G */
	dst[2] = clamp_byte(rgba[0] + bg[0] * transparency);  /* R */
	dst[3] = 0xff;
}

/**
 * blit_direct() - Copy the image at one destination pixel per source
 *                 pixel.
 * @image: decoded source image.
 * @buffer: destination buffer.
 * @g: geometry whose source and destination rectangles are the same size.
 * @bg: background colour as R,G,B floats.
 *
 * Used by "center" and by any crop that already matches the destination
 * exactly. There is nothing to resample, so this avoids the intermediate
 * buffer entirely and premultiplies each pixel as it is read.
 */
static void
blit_direct(const struct nyx_image *image, struct nyx_buffer *buffer,
            const struct nyx_geometry *g, const float bg[3])
{
	int32_t x, y;

	for (y = 0; y < g->dst_h; y++) {
		const unsigned char *src = image->pixels +
		    ((size_t)(g->src_y + y) * (size_t)image->width +
		     (size_t)g->src_x) * 4;
		unsigned char *dst = (unsigned char *)buffer->data +
		    (size_t)(g->dst_y + y) * (size_t)buffer->stride +
		    (size_t)g->dst_x * 4;

		for (x = 0; x < g->dst_w; x++) {
			float alpha = (float)src[3] / 255.0f;
			float rgba[4];

			rgba[0] = (float)src[0] * alpha;
			rgba[1] = (float)src[1] * alpha;
			rgba[2] = (float)src[2] * alpha;
			rgba[3] = (float)src[3];

			compose_pixel(dst, rgba, bg);

			src += 4;
			dst += 4;
		}
	}
}

/*
 * Hard ceiling on the resampler's intermediate, in bytes. The destination
 * is split into column strips narrow enough to keep the ring under this,
 * so peak memory is a constant of this file and not a function of anything
 * an image file or a compositor can choose. 64 MiB is far above what any
 * real configuration reaches -- a 2560-wide output off a 16384-tall source
 * needs about 250 KiB -- so the strip loop runs exactly once in practice.
 *
 * Overridable only so that a test can force the strip path with ordinary
 * image sizes; nothing in the program changes it.
 */
#ifndef NYX_RESAMPLE_ARENA_MAX
#define NYX_RESAMPLE_ARENA_MAX (64 * 1024 * 1024)
#endif

/**
 * resample_row_horizontal() - Run the horizontal pass for one source row.
 * @image: decoded source image.
 * @g: geometry giving the crop.
 * @axis: prebuilt horizontal weights.
 * @row: index of the row within the cropped region, 0 .. src_h - 1.
 * @x0: first destination column of the strip being built.
 * @xw: number of destination columns in the strip.
 * @out: @xw premultiplied RGBA pixels to write.
 *
 * Split out of blit_resampled() so that the vertical pass can ask for the
 * rows it needs one at a time instead of requiring the whole intermediate
 * to exist at once, and restricted to a column range so that the width of
 * that intermediate is bounded too.
 */
static void
resample_row_horizontal(const struct nyx_image *image,
                        const struct nyx_geometry *g,
                        const struct filter_axis *axis, int32_t row,
                        int32_t x0, int32_t xw, unsigned char *out)
{
	const unsigned char *src_row = image->pixels +
	    (size_t)(g->src_y + row) * (size_t)image->width * 4;
	int32_t x;

	for (x = 0; x < xw; x++) {
		const float *w = axis->weights +
		    (size_t)(x0 + x) * (size_t)axis->max_taps;
		int32_t start = axis->start[x0 + x];
		int32_t count = axis->count[x0 + x];
		float sum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		int32_t k;

		for (k = 0; k < count; k++) {
			const unsigned char *p = src_row + (size_t)(start + k) * 4;
			float weight = w[k];
			float alpha = (float)p[3] / 255.0f;

			sum[0] += weight * (float)p[0] * alpha;
			sum[1] += weight * (float)p[1] * alpha;
			sum[2] += weight * (float)p[2] * alpha;
			sum[3] += weight * (float)p[3];
		}

		out[x * 4 + 0] = clamp_byte(sum[0]);
		out[x * 4 + 1] = clamp_byte(sum[1]);
		out[x * 4 + 2] = clamp_byte(sum[2]);
		out[x * 4 + 3] = clamp_byte(sum[3]);
	}
}

/**
 * blit_resampled() - Scale the image into the buffer in two passes.
 * @image: decoded source image.
 * @buffer: destination buffer.
 * @g: geometry giving the crop and where it lands.
 * @bg: background colour as R,G,B floats.
 *
 * The horizontal pass turns a cropped source row into a dst_w premultiplied
 * row; the vertical pass combines those rows into the wl_buffer. Two
 * one-dimensional passes cost dst_w * src_h + dst_w * dst_h taps rather
 * than the product a two-dimensional kernel would need.
 *
 * The intermediate is a *ring* of rows over a *strip* of columns, not the
 * whole dst_w x src_h plane.
 *
 * The ring: a destination row reads at most vertical.max_taps source rows,
 * so a ring that size can never evict a row that is still needed, because
 * producing row r + max_taps is what overwrites row r and no single
 * destination row spans that far. Rows are produced lazily, in ascending
 * order, and the window is reset rather than trusted if it ever fails to
 * slide forward.
 *
 * The strip: the ring alone leaves the intermediate proportional to
 * src_h / dst_h, which is small for any real output but not *bounded* --
 * a one-pixel-tall surface would still ask for the whole source. Splitting
 * the destination into column strips narrow enough to fit
 * NYX_RESAMPLE_ARENA_MAX turns that into a constant. It costs nothing: each
 * strip does the horizontal pass only for its own columns, so the total tap
 * count is the same as one pass over the full width.
 *
 * Together they take peak memory off both the source's height and the
 * output's width. A 4096x32767 image on a 2560-wide, 1440-tall output held
 * a 320 MiB intermediate when this was a full plane; it now holds about
 * 250 KiB, and no input of any shape can push it past the ceiling.
 *
 * Return: 0 on success, -1 if either filter axis could not be built.
 */
static int
blit_resampled(const struct nyx_image *image, struct nyx_buffer *buffer,
               const struct nyx_geometry *g, const float bg[3])
{
	struct filter_axis horizontal, vertical;
	unsigned char *ring = NULL;
	float *accumulator = NULL;
	int32_t ring_rows, strip_w;
	int32_t x, y, x0;
	int result = -1;

	memset(&horizontal, 0, sizeof(horizontal));
	memset(&vertical, 0, sizeof(vertical));

	if (filter_axis_build(&horizontal, g->src_x, g->src_w, g->dst_w) < 0)
		goto out;
	if (filter_axis_build(&vertical, 0, g->src_h, g->dst_h) < 0)
		goto out;

	/* filter_axis_build() truncates count to max_taps and clamps the
	 * window to the source, so count <= min(max_taps, src_h) for every
	 * destination row. Sizing the ring to that bound is what makes
	 * eviction safe. */
	ring_rows = vertical.max_taps;
	if (ring_rows > g->src_h)
		ring_rows = g->src_h;

	/* Widest strip whose ring fits the ceiling, and never narrower than
	 * one column, which is what keeps this correct even at a ceiling
	 * smaller than a single row. */
	strip_w = (int32_t)(NYX_RESAMPLE_ARENA_MAX / ((size_t)ring_rows * 4));
	if (strip_w < 1)
		strip_w = 1;
	if (strip_w > g->dst_w)
		strip_w = g->dst_w;

	ring = nyx_alloc((size_t)strip_w * (size_t)ring_rows, 4);
	accumulator = nyx_alloc((size_t)strip_w, 4 * sizeof(float));

	for (x0 = 0; x0 < g->dst_w; x0 += strip_w) {
		int32_t xw = g->dst_w - x0 < strip_w ? g->dst_w - x0 : strip_w;
		int32_t have_lo = 0, have_hi = 0;

		for (y = 0; y < g->dst_h; y++) {
			const float *w = vertical.weights +
			    (size_t)y * (size_t)vertical.max_taps;
			int32_t need_lo = vertical.start[y];
			int32_t need_hi = need_lo + vertical.count[y];
			unsigned char *dst = (unsigned char *)buffer->data +
			    (size_t)(g->dst_y + y) * (size_t)buffer->stride +
			    (size_t)(g->dst_x + x0) * 4;
			int32_t r, k;

			/* The window only ever slides forward for a monotone
			 * filter, but the ring does not depend on that being
			 * true: anything else discards and refills. */
			if (need_lo < have_lo || need_lo > have_hi)
				have_lo = have_hi = need_lo;

			for (r = have_hi; r < need_hi; r++)
				resample_row_horizontal(image, g, &horizontal,
				    r, x0, xw,
				    ring + (size_t)(r % ring_rows) *
				           (size_t)strip_w * 4);

			if (need_hi > have_hi)
				have_hi = need_hi;
			if (have_hi - have_lo > ring_rows)
				have_lo = have_hi - ring_rows;

			memset(accumulator, 0,
			       (size_t)xw * 4 * sizeof(float));

			for (k = 0; k < vertical.count[y]; k++) {
				const unsigned char *tmp_row = ring +
				    (size_t)((need_lo + k) % ring_rows) *
				    (size_t)strip_w * 4;
				float weight = w[k];
				int32_t i;

				for (i = 0; i < xw * 4; i++)
					accumulator[i] +=
					    weight * (float)tmp_row[i];
			}

			for (x = 0; x < xw; x++)
				compose_pixel(dst + (size_t)x * 4,
				              accumulator + (size_t)x * 4, bg);
		}
	}

	result = 0;

out:
	free(accumulator);
	free(ring);
	filter_axis_finish(&horizontal);
	filter_axis_finish(&vertical);

	return result;
}

/**
 * draw() - Paint one buffer from the current image and settings.
 * @state: application state, for the image, mode and background colour.
 * @buffer: buffer to paint.
 *
 * The background is filled first and unconditionally, so the letterboxing
 * in "fit" and "center" needs no separate step and a buffer is never left
 * partly undefined.
 *
 * The 1:1 case is dispatched to blit_direct(), which is both faster and
 * exactly sharp; anything else goes through the resampler.
 *
 * A NULL image is not an error: the background alone is a valid frame.
 *
 * Return: 0 on success, -1 if the geometry could not be computed or the
 * resampler failed.
 */
static int
draw(const struct nyx_state *state, struct nyx_buffer *buffer)
{
	const struct nyx_image *image = state->image;
	struct nyx_geometry g;
	float bg[3];

	bg[0] = (float)((state->background >> 16) & 0xff);
	bg[1] = (float)((state->background >> 8) & 0xff);
	bg[2] = (float)(state->background & 0xff);

	fill_background(buffer, state->background);

	if (image == NULL)
		return 0;

	if (nyx_scale_compute(state->mode, image->width, image->height,
	                      buffer->width, buffer->height, &g) < 0) {
		nyx_log("cannot compute geometry for a %dx%d image on a %dx%d "
		        "output", image->width, image->height,
		        buffer->width, buffer->height);
		return -1;
	}

	if (g.src_w == g.dst_w && g.src_h == g.dst_h) {
		blit_direct(image, buffer, &g, bg);
		return 0;
	}

	return blit_resampled(image, buffer, &g, bg);
}

/* --------------------------------------------------------------------- */
/* Entry point                                                            */
/* --------------------------------------------------------------------- */

/**
 * nyx_render_layer() - Render the wallpaper for a layer surface and commit
 *                      it.
 * @state: application state, for the image, shm and compositor.
 * @layer: layer to render. Must have been configured.
 *
 * Both operands of the surface-size multiplication come from the
 * compositor. Multiplying them as int32_t would be undefined behaviour
 * before any bound could reject the result, so the product is formed in
 * 64 bits and narrowed only once it is known to fit.
 *
 * The opaque region is declared over the whole surface, which lets the
 * compositor skip blending: the buffer is XRGB8888 and covers everything.
 * Failing to create that region costs performance, not correctness, so it
 * is not treated as an error.
 *
 * Damage is posted in buffer coordinates where wl_surface version 4 allows
 * it, and in surface coordinates otherwise.
 *
 * Attaching the new buffer makes the previous one releasable. If the
 * compositor has already let go of it, it is reclaimed here; otherwise it
 * is marked doomed and buffer_handle_release() finishes the job.
 *
 * Return: 0 on success, -1 on failure. A failure leaves the previously
 * committed content on screen.
 */
int
nyx_render_layer(struct nyx_state *state, struct nyx_layer *layer)
{
	struct nyx_buffer *buffer, *previous;
	struct wl_region *opaque;
	int32_t scale, width, height;
	int64_t pixel_width, pixel_height;

	/* Both globals can go away under us: the registry may withdraw them
	 * at any point, and a request on a withdrawn object is discarded
	 * rather than diagnosed. Checking here is what turns that into a
	 * skipped frame instead of a NULL passed into libwayland. */
	if (state->shm == NULL || state->compositor == NULL ||
	    layer->surface == NULL)
		return -1;

	scale = layer->output->scale > 0 ? layer->output->scale : 1;

	pixel_width = (int64_t)layer->width * (int64_t)scale;
	pixel_height = (int64_t)layer->height * (int64_t)scale;

	if (pixel_width <= 0 || pixel_height <= 0) {
		nyx_log("layer surface has no size yet, skipping render");
		return -1;
	}
	if (pixel_width > NYX_MAX_BUFFER_DIMENSION ||
	    pixel_height > NYX_MAX_BUFFER_DIMENSION) {
		nyx_log("refusing a %ldx%ld pixel surface (%dx%d at scale %d)",
		        (long)pixel_width, (long)pixel_height,
		        layer->width, layer->height, scale);
		return -1;
	}

	/*
	 * Both axes are in range but the product need not be. Refusing here
	 * is what keeps a compositor from making this process hold half a
	 * gigabyte or more; see NYX_MAX_BUFFER_PIXELS for the size chosen and
	 * why an 8K panel at scale 2 still fits. The multiplication is in
	 * int64_t and both factors are already bounded, so it cannot wrap.
	 */
	if (pixel_width * pixel_height > NYX_MAX_BUFFER_PIXELS) {
		nyx_log("refusing a %ldx%ld pixel surface: %lld pixels is "
		        "above the %lld pixel buffer limit",
		        (long)pixel_width, (long)pixel_height,
		        (long long)(pixel_width * pixel_height),
		        (long long)NYX_MAX_BUFFER_PIXELS);
		return -1;
	}

	width = (int32_t)pixel_width;
	height = (int32_t)pixel_height;

	/*
	 * A buffer the compositor still holds cannot be freed, so a peer that
	 * never sends wl_buffer.release turns every configure into another
	 * live mapping. Alternating two sizes at protocol speed would grow
	 * the list without bound -- a few hundred events at 1920x1080 is
	 * gigabytes -- until nyx_fatal() or the OOM killer ends it, possibly
	 * against some other process.
	 *
	 * The cap is what makes that a dropped frame instead. A compositor
	 * that behaves holds one buffer, briefly two across a resize, so
	 * reaching four means it has stopped releasing and there is nothing
	 * useful left to draw for it. What is already on screen stays.
	 */
	if (wl_list_length(&layer->buffers) >= NYX_MAX_LAYER_BUFFERS) {
		nyx_log("output %s: %d buffers still held by the compositor, "
		        "skipping this frame",
		        layer->output->name != NULL ? layer->output->name : "?",
		        NYX_MAX_LAYER_BUFFERS);
		return -1;
	}

	buffer = buffer_create(state->shm, width, height);
	if (buffer == NULL)
		return -1;

	buffer->layer = layer;
	wl_list_insert(&layer->buffers, &buffer->link);

	if (draw(state, buffer) < 0) {
		nyx_buffer_destroy(buffer);
		return -1;
	}

	if (wl_proxy_get_version((struct wl_proxy *)layer->surface) >= 3)
		wl_surface_set_buffer_scale(layer->surface, scale);

	opaque = wl_compositor_create_region(state->compositor);
	if (opaque != NULL) {
		wl_region_add(opaque, 0, 0, layer->width, layer->height);
		wl_surface_set_opaque_region(layer->surface, opaque);
		wl_region_destroy(opaque);
	}

	wl_surface_attach(layer->surface, buffer->wl_buffer, 0, 0);

	if (wl_proxy_get_version((struct wl_proxy *)layer->surface) >= 4)
		wl_surface_damage_buffer(layer->surface, 0, 0, width, height);
	else
		wl_surface_damage(layer->surface, 0, 0,
		                  layer->width, layer->height);

	wl_surface_commit(layer->surface);

	buffer->busy = 1;

	previous = layer->buffer;
	layer->buffer = buffer;
	layer->scale = scale;

	if (previous != NULL) {
		previous->doomed = 1;
		if (!previous->busy)
			nyx_buffer_destroy(previous);
	}

	nyx_debug("rendered %dx%d at scale %d on output %s",
	          width, height, scale,
	          layer->output->name != NULL ? layer->output->name : "?");

	return 0;
}
