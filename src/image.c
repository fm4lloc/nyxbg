// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NyxBG - PNG and JPEG decoding.
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
 * Both decoders converge on one output format: 8 bits per channel, RGBA
 * byte order, non-premultiplied, tightly packed. Everything downstream can
 * then assume a single layout.
 *
 * The container type is decided by magic bytes. A file named .png that is
 * really a JPEG decodes correctly; a file named .png that is neither is
 * rejected with a clear message rather than handed to libpng.
 *
 * The image file is the program's one untrusted input, so both decoders
 * bound the dimensions before allocating and both report errors through
 * setjmp/longjmp, which is the only error mechanism libpng and libjpeg
 * offer. Variables whose values must survive a longjmp() are declared
 * volatile; the rest are not read on the cleanup path.
 */

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <jpeglib.h>
#include <png.h>

#include "image.h"
#include "util.h"

/*
 * Bounds on what will be decoded.  These exist to turn a corrupt or
 * hostile header into a clean error instead of an allocation the size of
 * the address space.  The pixel cap corresponds to 512 MiB of RGBA.
 */
#define NYX_IMAGE_MAX_DIMENSION 32767
#define NYX_IMAGE_MAX_PIXELS    ((int64_t)134217728)

/*
 * Bounds on what libpng may allocate for ancillary chunks before the image
 * has even been described.  libpng's own defaults are ~8 MB per chunk with
 * up to 1000 cached, which lets a small file with a few hundred compressed
 * text chunks retain gigabytes: the decompressed text is held until the
 * read struct is destroyed, and libpng reports a failed allocation there as
 * a *warning*, so the decode still succeeds and there is no error path to
 * fall back on.  A 771 KB file declaring a 1x1 image measured 789 MiB of
 * peak RSS against the defaults and still returned a valid 1x1 image.
 *
 * NyxBG reads no text, no ICC profile and no EXIF, so it wants the smallest
 * budget that still lets a normal wallpaper through.  32 chunks of 256 KiB
 * caps the retained total at 8 MiB, which is far above what any real file
 * carries -- a colour profile is the largest thing a wallpaper plausibly
 * has, and those are single-digit KiB.
 */
#define NYX_PNG_MAX_CHUNK_BYTES  ((png_alloc_size_t)(256 * 1024))
#define NYX_PNG_MAX_CHUNK_COUNT  32

/**
 * enum image_format - Container type identified from a file's magic bytes.
 * @IMAGE_FORMAT_UNKNOWN: neither PNG nor JPEG, or the file is too short.
 * @IMAGE_FORMAT_PNG: the eight-byte PNG signature was found.
 * @IMAGE_FORMAT_JPEG: a JPEG start-of-image marker was found.
 */
enum image_format {
	IMAGE_FORMAT_UNKNOWN,
	IMAGE_FORMAT_PNG,
	IMAGE_FORMAT_JPEG
};

static const unsigned char png_magic[8] = {
	0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
};

/* --------------------------------------------------------------------- */
/* Shared helpers                                                         */
/* --------------------------------------------------------------------- */

/**
 * dimensions_are_sane() - Check decoded dimensions against the limits.
 * @path: file name, for the diagnostic.
 * @width: width reported by the decoder.
 * @height: height reported by the decoder.
 *
 * Called on the declared size as soon as the decoder has parsed it, and
 * before the call that would allocate from it -- which is the whole point:
 * a hostile header should cost an error message, not an allocation. For
 * JPEG that means before jpeg_start_decompress(), which builds the
 * coefficient array for a progressive image; for PNG, before
 * png_read_image().
 *
 * The parameters are int64_t rather than long so that @width * @height
 * cannot overflow on a platform where long is 32 bits, independently of
 * what the limits above are set to.
 *
 * Return: 1 if the dimensions are acceptable, 0 otherwise. A diagnostic
 * has already been printed when 0 is returned.
 */
static int
dimensions_are_sane(const char *path, int64_t width, int64_t height)
{
	if (width <= 0 || height <= 0) {
		nyx_log("%s: image has no pixels", path);
		return 0;
	}
	if (width > NYX_IMAGE_MAX_DIMENSION || height > NYX_IMAGE_MAX_DIMENSION) {
		nyx_log("%s: image is %ldx%ld, larger than the %d pixel limit "
		        "on either axis", path, (long)width, (long)height,
		        NYX_IMAGE_MAX_DIMENSION);
		return 0;
	}
	if (width * height > NYX_IMAGE_MAX_PIXELS) {
		nyx_log("%s: image is %ldx%ld, above the %ld pixel total limit",
		        path, (long)width, (long)height,
		        (long)NYX_IMAGE_MAX_PIXELS);
		return 0;
	}

	return 1;
}

/**
 * image_alloc() - Allocate an image and its pixel buffer.
 * @width: width in pixels; must have passed dimensions_are_sane().
 * @height: height in pixels; must have passed dimensions_are_sane().
 *
 * The product is safe to form in size_t only because the caller has
 * already bounded both axes and their product.
 *
 * Return: A zeroed image of the requested size. Never NULL; allocation
 * failure calls nyx_fatal() and does not return.
 */
static struct nyx_image *
image_alloc(int32_t width, int32_t height)
{
	struct nyx_image *image;

	image = nyx_alloc(1, sizeof(*image));
	image->width = width;
	image->height = height;
	image->pixels = nyx_alloc((size_t)width * (size_t)height, 4);

	return image;
}

/**
 * nyx_image_destroy() - Release a decoded image.
 * @image: image to release. May be NULL, in which case nothing happens.
 */
void
nyx_image_destroy(struct nyx_image *image)
{
	if (image == NULL)
		return;

	free(image->pixels);
	free(image);
}

/**
 * detect_format() - Identify a container from its leading bytes.
 * @fp: stream positioned at the start of the file.
 * @path: file name, for the diagnostic.
 *
 * The extension is deliberately ignored. A wallpaper may arrive from
 * anywhere and its name says nothing reliable about its contents; the
 * magic bytes do.
 *
 * The stream is rewound before returning, so the chosen decoder receives
 * it exactly as it was handed in.
 *
 * Return: The detected format, or %IMAGE_FORMAT_UNKNOWN if the file
 * matches neither signature or could not be rewound.
 */
static enum image_format
detect_format(FILE *fp, const char *path)
{
	unsigned char magic[8];
	size_t n;

	n = fread(magic, 1, sizeof(magic), fp);
	if (fseek(fp, 0L, SEEK_SET) != 0) {
		nyx_log("%s: cannot rewind file", path);
		return IMAGE_FORMAT_UNKNOWN;
	}

	if (n >= sizeof(png_magic) &&
	    memcmp(magic, png_magic, sizeof(png_magic)) == 0)
		return IMAGE_FORMAT_PNG;

	/* SOI marker followed by any marker byte. */
	if (n >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)
		return IMAGE_FORMAT_JPEG;

	return IMAGE_FORMAT_UNKNOWN;
}

/* --------------------------------------------------------------------- */
/* PNG                                                                    */
/* --------------------------------------------------------------------- */

/**
 * struct png_error_context - Where libpng jumps to, and what to call it.
 * @jump: destination for longjmp() out of libpng's error handler.
 * @path: file name, for the diagnostic.
 *
 * Reached from the callbacks through png_get_error_ptr(), which is how
 * libpng passes caller state into an error handler.
 */
struct png_error_context {
	jmp_buf jump;
	const char *path;
};

/**
 * png_on_error() - Report a fatal libpng error and unwind.
 * @png: reader the error came from.
 * @message: libpng's description of the problem.
 *
 * libpng requires that an error callback does not return; doing so is
 * undefined. The longjmp() lands in load_png(), which owns the cleanup.
 *
 * Return: Never returns.
 */
static _Noreturn void
png_on_error(png_structp png, png_const_charp message)
{
	struct png_error_context *ctx = png_get_error_ptr(png);

	nyx_log("%s: PNG error: %s", ctx->path, message);
	longjmp(ctx->jump, 1);
}

/**
 * png_on_warning() - Report a non-fatal libpng warning.
 * @png: reader the warning came from.
 * @message: libpng's description of the problem.
 *
 * Warnings are informational: libpng carries on, so this returns normally
 * and the message is only shown under --verbose. Suppressing them entirely
 * would hide genuinely malformed files that still decode.
 */
static void
png_on_warning(png_structp png, png_const_charp message)
{
	struct png_error_context *ctx = png_get_error_ptr(png);

	nyx_debug("%s: PNG warning: %s", ctx->path, message);
}

/**
 * load_png() - Decode a PNG stream into canonical RGBA.
 * @fp: stream positioned at the start of the file.
 * @path: file name, for diagnostics.
 *
 * Every PNG flavour is normalised to 8-bit RGBA by libpng's own
 * transformations: 16-bit samples are stripped to 8, palettes and low-bit
 * greyscale are expanded, a tRNS chunk becomes a real alpha channel,
 * greyscale becomes RGB, and png_set_filler() supplies an opaque alpha
 * byte where the source has none. The row size is then asserted to be
 * exactly four bytes per pixel, so the rest of the program can index the
 * buffer without consulting libpng again.
 *
 * @image and @rows are volatile because they are assigned after setjmp()
 * and read on the longjmp() path; the remaining locals are either fixed
 * before the jump target or never read after it.
 *
 * Return: A newly allocated image, or NULL on failure. Diagnostics have
 * already been printed on failure.
 */
static struct nyx_image *
load_png(FILE *fp, const char *path)
{
	struct png_error_context ctx;
	png_structp png = NULL;
	png_infop info = NULL;
	/* Touched after setjmp(), hence volatile: their values must survive
	 * a longjmp() out of libpng so the cleanup path can free them. */
	struct nyx_image * volatile image = NULL;
	png_bytep * volatile rows = NULL;
	png_uint_32 width, height;
	int bit_depth, color_type;
	png_uint_32 y;

	ctx.path = path;

	png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &ctx,
	                             png_on_error, png_on_warning);
	if (png == NULL) {
		nyx_log("%s: cannot allocate PNG reader", path);
		return NULL;
	}

	info = png_create_info_struct(png);
	if (info == NULL) {
		nyx_log("%s: cannot allocate PNG info", path);
		png_destroy_read_struct(&png, NULL, NULL);
		return NULL;
	}

	if (setjmp(ctx.jump)) {
		free(rows);
		nyx_image_destroy(image);
		png_destroy_read_struct(&png, &info, NULL);
		return NULL;
	}

	/*
	 * Set before png_read_info(), which is where ancillary chunks are
	 * read and where an unbudgeted decode would spend its memory.  See
	 * the constants above for why these numbers.
	 */
	png_set_chunk_malloc_max(png, NYX_PNG_MAX_CHUNK_BYTES);
	png_set_chunk_cache_max(png, NYX_PNG_MAX_CHUNK_COUNT);

	png_init_io(png, fp);
	png_read_info(png, info);

	width = png_get_image_width(png, info);
	height = png_get_image_height(png, info);
	bit_depth = png_get_bit_depth(png, info);
	color_type = png_get_color_type(png, info);

	if (!dimensions_are_sane(path, (int64_t)width, (int64_t)height))
		longjmp(ctx.jump, 1);

	/* Normalise every PNG flavour to 8-bit RGBA. */
	if (bit_depth == 16)
		png_set_strip_16(png);
	if (color_type == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (color_type == PNG_COLOR_TYPE_GRAY ||
	    color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);

	/* Adds an opaque alpha byte when the source has none.  Harmless
	 * when alpha is already present: libpng ignores it. */
	png_set_filler(png, 0xff, PNG_FILLER_AFTER);

	(void)png_set_interlace_handling(png);
	png_read_update_info(png, info);

	if (png_get_rowbytes(png, info) != (png_size_t)width * 4) {
		nyx_log("%s: unexpected PNG row size after conversion", path);
		longjmp(ctx.jump, 1);
	}

	image = image_alloc((int32_t)width, (int32_t)height);

	rows = nyx_alloc(height, sizeof(png_bytep));
	for (y = 0; y < height; y++)
		rows[y] = image->pixels + (size_t)y * (size_t)width * 4;

	png_read_image(png, rows);
	png_read_end(png, NULL);

	free(rows);
	png_destroy_read_struct(&png, &info, NULL);

	return image;
}

/* --------------------------------------------------------------------- */
/* JPEG                                                                   */
/* --------------------------------------------------------------------- */

/**
 * struct jpeg_error_context - libjpeg's error manager, extended.
 * @base: the standard manager; must be the first member so that a
 *        j_common_ptr's err field can be cast back to this struct.
 * @jump: destination for longjmp() out of libjpeg's error handler.
 * @path: file name, for the diagnostic.
 */
struct jpeg_error_context {
	struct jpeg_error_mgr base;
	jmp_buf jump;
	const char *path;
};

/**
 * jpeg_on_error() - Report a fatal libjpeg error and unwind.
 * @cinfo: decompressor the error came from.
 *
 * libjpeg's default error_exit calls exit(), which a wallpaper renderer
 * must not do on a bad file. Replacing it with a longjmp() turns a
 * malformed image into a return value.
 *
 * Return: Never returns.
 */
static _Noreturn void
jpeg_on_error(j_common_ptr cinfo)
{
	struct jpeg_error_context *ctx = (struct jpeg_error_context *)cinfo->err;
	char message[JMSG_LENGTH_MAX];

	ctx->base.format_message(cinfo, message);
	nyx_log("%s: JPEG error: %s", ctx->path, message);

	longjmp(ctx->jump, 1);
}

/**
 * jpeg_on_message() - Report a non-fatal libjpeg message.
 * @cinfo: decompressor the message came from.
 *
 * Corrupt-but-recoverable data reaches here. Decoding continues, so this
 * returns normally and the message is only shown under --verbose.
 */
static void
jpeg_on_message(j_common_ptr cinfo)
{
	struct jpeg_error_context *ctx = (struct jpeg_error_context *)cinfo->err;
	char message[JMSG_LENGTH_MAX];

	ctx->base.format_message(cinfo, message);
	nyx_debug("%s: JPEG: %s", ctx->path, message);
}

/**
 * load_jpeg() - Decode a JPEG stream into canonical RGBA.
 * @fp: stream positioned at the start of the file.
 * @path: file name, for diagnostics.
 *
 * libjpeg is asked for plain JCS_RGB rather than one of the JCS_EXT_*
 * colour spaces: the extended ones would let libjpeg write RGBA directly,
 * but they exist only in libjpeg-turbo, and the per-pixel expansion below
 * is cheap next to the decode itself. Greyscale and YCbCr sources are
 * converted by libjpeg on the way out, so only the three-component case
 * has to be handled here.
 *
 * @image and @scanline are volatile because they are assigned after
 * setjmp() and read on the longjmp() path.
 *
 * The decompressor lives on the heap rather than on the stack, which is
 * not a style choice. C11 7.13.2.1p3 makes an automatic, non-volatile
 * object indeterminate after a longjmp() if it was modified since the
 * setjmp(), and jpeg_create_decompress() modifies the struct while the
 * cleanup path reads it back through jpeg_destroy_decompress(). Adding
 * volatile is not available as a fix: &cinfo would then be a pointer to
 * volatile, which does not convert to j_decompress_ptr without casting
 * the qualifier away again. Allocated storage is outside the rule, and
 * the pointer to it is fixed before the setjmp() and never reassigned,
 * so the pointer itself stays determinate.
 *
 * Return: A newly allocated image, or NULL on failure. Diagnostics have
 * already been printed on failure.
 */
static struct nyx_image *
load_jpeg(FILE *fp, const char *path)
{
	struct jpeg_decompress_struct *cinfo;
	struct jpeg_error_context ctx;
	struct nyx_image * volatile image = NULL;
	unsigned char * volatile scanline = NULL;
	JSAMPROW row_pointer[1];
	int32_t width, height;

	/*
	 * libjpeg sets cinfo->mem to NULL as the first statement of
	 * jpeg_CreateDecompress precisely so that jpeg_destroy_decompress is
	 * safe on an early error_exit, so the zeroing below is not
	 * load-bearing.  It keeps the struct from being read while
	 * uninitialised at all: jpeg_CreateDecompress preserves client_data
	 * across its own memset, which reads a field this program never sets.
	 * nyx_alloc() zeroes, so no separate memset is needed.
	 */
	cinfo = nyx_alloc(1, sizeof(*cinfo));

	cinfo->err = jpeg_std_error(&ctx.base);
	ctx.base.error_exit = jpeg_on_error;
	ctx.base.output_message = jpeg_on_message;
	ctx.path = path;

	if (setjmp(ctx.jump)) {
		jpeg_destroy_decompress(cinfo);
		free(cinfo);
		free(scanline);
		nyx_image_destroy(image);
		return NULL;
	}

	jpeg_create_decompress(cinfo);
	jpeg_stdio_src(cinfo, fp);

	if (jpeg_read_header(cinfo, TRUE) != JPEG_HEADER_OK) {
		nyx_log("%s: not a complete JPEG image", path);
		longjmp(ctx.jump, 1);
	}

	/*
	 * Bound the declared size here, on image_width/image_height, and not
	 * only on the output size below.  jpeg_start_decompress() is what
	 * allocates, and for a progressive JPEG it allocates the whole-image
	 * coefficient array before returning: a 192-byte file declaring
	 * 20000x20000 measured 2.2 GiB of peak RSS when this check lived only
	 * after the call.  jpeg_read_header() has already filled these two
	 * fields in, so the check costs nothing where it belongs.
	 */
	if (!dimensions_are_sane(path, (int64_t)cinfo->image_width,
	                         (int64_t)cinfo->image_height))
		longjmp(ctx.jump, 1);

	/* libjpeg converts YCbCr and greyscale sources for us; asking for
	 * plain RGB keeps this path portable to non-turbo libjpeg, which
	 * lacks the JCS_EXT_* colour spaces. */
	cinfo->out_color_space = JCS_RGB;

	jpeg_start_decompress(cinfo);

	width = (int32_t)cinfo->output_width;
	height = (int32_t)cinfo->output_height;

	/* Checked again on the output size: libjpeg may scale, and the two
	 * are only equal because this program never asks it to. */
	if (!dimensions_are_sane(path, (int64_t)width, (int64_t)height))
		longjmp(ctx.jump, 1);

	if (cinfo->output_components != 3) {
		nyx_log("%s: unexpected JPEG component count %d", path,
		        cinfo->output_components);
		longjmp(ctx.jump, 1);
	}

	image = image_alloc(width, height);
	scanline = nyx_alloc((size_t)width, 3);

	while (cinfo->output_scanline < cinfo->output_height) {
		unsigned char *dst;
		int32_t x;
		int32_t y = (int32_t)cinfo->output_scanline;

		row_pointer[0] = scanline;
		if (jpeg_read_scanlines(cinfo, row_pointer, 1) != 1) {
			nyx_log("%s: truncated JPEG image data", path);
			longjmp(ctx.jump, 1);
		}

		dst = image->pixels + (size_t)y * (size_t)width * 4;
		for (x = 0; x < width; x++) {
			dst[x * 4 + 0] = scanline[x * 3 + 0];
			dst[x * 4 + 1] = scanline[x * 3 + 1];
			dst[x * 4 + 2] = scanline[x * 3 + 2];
			dst[x * 4 + 3] = 0xff;
		}
	}

	jpeg_finish_decompress(cinfo);
	jpeg_destroy_decompress(cinfo);

	free(cinfo);
	free(scanline);

	return image;
}

/* --------------------------------------------------------------------- */
/* Entry point                                                            */
/* --------------------------------------------------------------------- */

/**
 * nyx_image_load() - Load and decode a PNG or JPEG file.
 * @path: path to the file. May be NULL, which is treated as a failure.
 *
 * The file must be a regular file: a directory, a device or a FIFO is
 * rejected, because a wallpaper path that names one is a mistake and
 * reading a FIFO would block the process indefinitely.
 *
 * The check is a stat() before the fopen(), so it is advisory rather than
 * airtight: a path replaced between the two calls would be opened anyway.
 * Closing that window needs open() followed by fstat() on the descriptor.
 * It is left as is because the path comes from the invoking user's own
 * command line, which puts the attacker and the victim in the same trust
 * domain.
 *
 * The container type comes from the file's magic bytes, never from its
 * extension.
 *
 * Return: A newly allocated image on success, NULL on failure. Failures
 * are reported to stderr here, so the caller only needs to react to the
 * NULL return.
 */
struct nyx_image *
nyx_image_load(const char *path)
{
	struct nyx_image *image = NULL;
	struct stat st;
	FILE *fp;

	if (path == NULL) {
		/* Unreachable: main() rejects a missing path before it gets
		 * here. Diagnosed anyway, so that the documented postcondition
		 * -- every failure prints something -- holds on every branch. */
		nyx_log("no image path given");
		return NULL;
	}

	if (stat(path, &st) != 0) {
		nyx_log("%s: cannot stat file", path);
		return NULL;
	}
	if (!S_ISREG(st.st_mode)) {
		nyx_log("%s: not a regular file", path);
		return NULL;
	}

	fp = fopen(path, "rb");
	if (fp == NULL) {
		nyx_log("%s: cannot open file", path);
		return NULL;
	}

	switch (detect_format(fp, path)) {
	case IMAGE_FORMAT_PNG:
		image = load_png(fp, path);
		break;
	case IMAGE_FORMAT_JPEG:
		image = load_jpeg(fp, path);
		break;
	case IMAGE_FORMAT_UNKNOWN:
	default:
		nyx_log("%s: unrecognised image format, expected PNG or JPEG",
		        path);
		break;
	}

	fclose(fp);

	if (image != NULL)
		nyx_debug("loaded %s (%dx%d)", path, image->width, image->height);

	return image;
}
