// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NyxBG - Generic helper routines.
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
 * Nothing in this file knows about Wayland, images or rendering. The only
 * state is the verbose flag, set once from the command line before any
 * other module runs, so no synchronisation is involved.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Non-zero once --verbose has been seen; gates nyx_debug() only. */
static int verbose_enabled;

/* --------------------------------------------------------------------- */
/* Diagnostics                                                            */
/* --------------------------------------------------------------------- */

/**
 * vlog() - Write one prefixed diagnostic line to stderr.
 * @fmt: printf-style format string.
 * @ap: arguments matching @fmt.
 *
 * The trailing newline is supplied here, so callers never write one. All
 * diagnostics go to stderr, including those emitted on the success path,
 * so that stdout stays free for --help and --version.
 *
 * Context: Not async-signal-safe. Must not be called from a signal
 * handler.
 */
static void NYX_PRINTF(1, 0)
vlog(const char *fmt, va_list ap)
{
	fputs("nyxbg: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

/**
 * nyx_log_set_verbose() - Enable or disable nyx_debug() output.
 * @verbose: non-zero to enable, zero to disable.
 *
 * Verbose output is disabled until this is called. Argument parsing calls
 * it before any other module starts, and nothing calls it afterwards.
 */
void
nyx_log_set_verbose(int verbose)
{
	verbose_enabled = verbose;
}

/**
 * nyx_log() - Report a condition the user needs to know about.
 * @fmt: printf-style format string, without a trailing newline.
 *
 * Used for errors and warnings. Always printed, regardless of --verbose.
 */
void
nyx_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

/**
 * nyx_debug() - Report a detail that is only interesting when debugging.
 * @fmt: printf-style format string, without a trailing newline.
 *
 * Discarded unless nyx_log_set_verbose() has enabled verbose output.
 */
void
nyx_debug(const char *fmt, ...)
{
	va_list ap;

	if (!verbose_enabled)
		return;

	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);
}

/**
 * nyx_fatal() - Report a condition with no defined recovery and exit.
 * @fmt: printf-style format string, without a trailing newline.
 *
 * Reserved for memory exhaustion, which is the one failure this program
 * has no meaningful answer to. Every other failure is reported through a
 * return value so the caller can decide. The process exits with
 * EXIT_FAILURE without unwinding: nothing needs releasing that the kernel
 * will not release anyway.
 *
 * Return: Never returns.
 */
_Noreturn void
nyx_fatal(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vlog(fmt, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}

/* --------------------------------------------------------------------- */
/* Memory                                                                 */
/* --------------------------------------------------------------------- */

/**
 * nyx_alloc() - Allocate zero-initialised memory or die trying.
 * @count: number of elements.
 * @size: size of one element, in bytes.
 *
 * calloc() with the failure case removed from every call site. A zero
 * @count or @size is rounded up to a single byte, so the returned pointer
 * is always distinct and always safe to free().
 *
 * The overflow check that calloc() performs on @count * @size is relied on
 * rather than repeated here.
 *
 * Return: A pointer to @count * @size zeroed bytes. Never NULL; allocation
 * failure calls nyx_fatal() and does not return.
 */
void *
nyx_alloc(size_t count, size_t size)
{
	void *p;

	if (count == 0 || size == 0)
		count = size = 1;

	p = calloc(count, size);
	if (p == NULL)
		nyx_fatal("out of memory");

	return p;
}

/**
 * nyx_strdup() - Duplicate a NUL-terminated string.
 * @s: string to copy. Must not be NULL.
 *
 * Return: A newly allocated copy of @s, to be released with free(). Never
 * NULL; allocation failure calls nyx_fatal() and does not return.
 */
char *
nyx_strdup(const char *s)
{
	size_t len;
	char *copy;

	len = strlen(s);
	copy = nyx_alloc(len + 1, 1);
	memcpy(copy, s, len);

	return copy;
}

/* --------------------------------------------------------------------- */
/* Parsing                                                                */
/* --------------------------------------------------------------------- */

/**
 * hex_digit() - Convert one hexadecimal character to its value.
 * @c: character to convert; either case is accepted.
 *
 * Written out rather than delegated to isxdigit() and strtoul() because
 * those follow the locale and would accept forms this program does not
 * want, such as a leading sign or surrounding space.
 *
 * Return: The value 0 to 15, or -1 if @c is not a hexadecimal digit.
 */
static int
hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return -1;
}

/**
 * nyx_parse_color() - Parse an RGB colour written in hexadecimal.
 * @s: colour text, "RRGGBB" or "#RRGGBB". May be NULL.
 * @out: receives the colour as 0x00RRGGBB. May be NULL.
 *
 * Exactly six digits are required and nothing may follow them, so a
 * truncated or over-long argument is rejected rather than silently
 * reinterpreted. @out is left untouched when the string is rejected.
 *
 * Return: 0 on success, -1 if @s is not a valid colour.
 */
int
nyx_parse_color(const char *s, uint32_t *out)
{
	uint32_t value = 0;
	int i;

	if (s == NULL || out == NULL)
		return -1;

	if (*s == '#')
		s++;

	for (i = 0; i < 6; i++) {
		int digit = hex_digit(s[i]);

		if (digit < 0)
			return -1;
		value = (value << 4) | (uint32_t)digit;
	}

	if (s[6] != '\0')
		return -1;

	*out = value;

	return 0;
}

/* --------------------------------------------------------------------- */
/* Arithmetic                                                             */
/* --------------------------------------------------------------------- */

/**
 * nyx_scale_ratio() - Compute a * b / c without overflowing.
 * @a: first factor.
 * @b: second factor.
 * @c: divisor.
 *
 * The product is formed in 64 bits, so no pair of int32_t inputs can
 * overflow it, and the division rounds to nearest rather than towards
 * zero. This is the one arithmetic primitive the geometry in scale.c is
 * built from; keeping it here means the overflow argument is made once.
 *
 * A non-positive argument is a caller error rather than a value with a
 * meaningful answer, and yields 1.
 *
 * Return: The rounded quotient, clamped to the range 1 to INT32_MAX.
 */
int32_t
nyx_scale_ratio(int32_t a, int32_t b, int32_t c)
{
	int64_t product, result;

	if (a <= 0 || b <= 0 || c <= 0)
		return 1;

	product = (int64_t)a * (int64_t)b;
	result = (product + c / 2) / c;

	if (result < 1)
		result = 1;
	if (result > INT32_MAX)
		result = INT32_MAX;

	return (int32_t)result;
}
