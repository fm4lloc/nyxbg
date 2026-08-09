/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * NyxBG - Signal handling.
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
 * Signals are converted into readable bytes on a self-pipe so that they
 * can be waited on by the same poll() call as the Wayland connection. The
 * installed handlers perform one async-signal-safe write() and nothing
 * else; all interpretation happens in the event loop.
 *
 * The module keeps one pipe as file-scope state, because a signal handler
 * has no other way to reach it. It is single-instance by nature: there is
 * one process, one disposition per signal.
 *
 * This header is deliberately named signal.h, shadowing the system header
 * of that name. The Makefile adds include/ with -iquote rather than -I, so
 * "signal.h" finds this file and <signal.h> still finds libc's.
 */
#ifndef NYXBG_SIGNAL_H
#define NYXBG_SIGNAL_H

int nyx_signal_init(void);

int nyx_signal_poll(int *terminate, int *reload);

void nyx_signal_finish(void);

#endif /* NYXBG_SIGNAL_H */
