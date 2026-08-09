/* SPDX-License-Identifier: GPL-3.0-or-later */
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
 * This module contains no Wayland-specific logic and depends on nothing
 * beyond the C standard library. It owns one piece of state, the verbose
 * flag, which is written once during argument parsing and only read
 * afterwards.
 */
#ifndef NYXBG_UTIL_H
#define NYXBG_UTIL_H

#include <stddef.h>
#include <stdint.h>

/*
 * Function attributes. GCC and Clang both understand these; anything else
 * loses the diagnostics and keeps the semantics, so the code stays
 * portable to a compiler that has neither.
 */
#if defined(__GNUC__)
#define NYX_PRINTF(fmt_index, first_arg) \
	__attribute__((format(printf, fmt_index, first_arg)))
#define NYX_MALLOC __attribute__((malloc))
#else
#define NYX_PRINTF(fmt_index, first_arg)
#define NYX_MALLOC
#endif

/* Diagnostics ------------------------------------------------------------ */

void nyx_log_set_verbose(int verbose);
void nyx_log(const char *fmt, ...) NYX_PRINTF(1, 2);
void nyx_debug(const char *fmt, ...) NYX_PRINTF(1, 2);
_Noreturn void nyx_fatal(const char *fmt, ...) NYX_PRINTF(1, 2);

/* Memory ----------------------------------------------------------------- */

NYX_MALLOC void *nyx_alloc(size_t count, size_t size);
NYX_MALLOC char *nyx_strdup(const char *s);

/* Parsing ---------------------------------------------------------------- */

int nyx_parse_color(const char *s, uint32_t *out);

/* Arithmetic ------------------------------------------------------------- */

int32_t nyx_scale_ratio(int32_t a, int32_t b, int32_t c);

#endif /* NYXBG_UTIL_H */
