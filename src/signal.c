// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * NyxBG - SIGINT, SIGTERM and SIGHUP handling.
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
 * The classic self-pipe trick. The handler performs a single write() of
 * one byte -- the only non-trivial async-signal-safe operation involved --
 * and the event loop turns those bytes back into intent. This keeps the
 * signal path free of any shared state beyond a file descriptor, and lets
 * poll() wait on signals and Wayland events at the same time.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "signal.h"
#include "util.h"

/*
 * The read end is only ever touched by the event loop.  The write end is
 * read inside the handler, so it is sig_atomic_t: C11 7.14.1.1p5 permits a
 * handler to access exactly two kinds of object, and a plain int is not
 * one of them even when nothing races on it.
 */
static int pipe_read_fd = -1;
static volatile sig_atomic_t pipe_write_fd = -1;

/*
 * What the handler actually records.  The pipe carries two independent
 * classes of signal, so a byte dropped because the pipe was full is not
 * harmless: a burst of SIGHUP can fill the pipe while the process is busy
 * decoding, and the SIGINT that follows would then be lost and the process
 * would refuse to die.  These two flags are the record; the pipe is only
 * how the event loop is woken.  Setting a flag cannot fail.
 */
static volatile sig_atomic_t saw_terminate;
static volatile sig_atomic_t saw_reload;

/* The set this module installs handlers for, and restores on shutdown. */
static const int handled_signals[] = { SIGINT, SIGTERM, SIGHUP };

#define HANDLED_COUNT \
	((int)(sizeof(handled_signals) / sizeof(handled_signals[0])))

/* Dispositions found before this module replaced them, so shutdown can put
 * back what was there rather than assuming it was SIG_DFL.  A shell that
 * starts the program under nohup leaves SIGHUP as SIG_IGN, and clobbering
 * that on the way out would change the caller's environment. */
static struct sigaction previous_action[HANDLED_COUNT];
static int previous_valid[HANDLED_COUNT];

/**
 * handle_signal() - Record a delivered signal and wake the event loop.
 * @signo: signal number being delivered.
 *
 * Runs in signal context, so it does exactly two things: set the flag for
 * the class of signal that arrived, and write one byte to the self-pipe so
 * that poll() returns. Both are async-signal-safe; nothing else here would
 * be. errno is saved and restored because the interrupted code is entitled
 * to find it unchanged.
 *
 * The write is best effort. A full pipe means the event loop has not
 * drained yet and is already going to wake, and the flag above has already
 * recorded what happened, so a dropped byte costs nothing. That is only
 * true because the flag, not the byte, is what carries the information.
 *
 * Context: Signal handler. Async-signal-safe.
 */
static void
handle_signal(int signo)
{
	unsigned char byte = (unsigned char)signo;
	int saved_errno = errno;
	int fd = (int)pipe_write_fd;
	ssize_t written;

	if (signo == SIGHUP)
		saw_reload = 1;
	else
		saw_terminate = 1;

	if (fd >= 0) {
		do {
			written = write(fd, &byte, 1);
		} while (written < 0 && errno == EINTR);
	}

	errno = saved_errno;
}

/**
 * set_flags() - Make one end of the self-pipe close-on-exec and
 *               non-blocking.
 * @fd: descriptor to configure.
 *
 * Non-blocking matters on both ends for different reasons: the write end
 * so a handler firing against a full pipe cannot block inside signal
 * context, and the read end so the drain loop can stop at EAGAIN instead
 * of sleeping. Close-on-exec is hygiene; this program never execs.
 *
 * Return: 0 on success, -1 with errno set on failure.
 */
static int
set_flags(int fd)
{
	int flags;

	flags = fcntl(fd, F_GETFD);
	if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0)
		return -1;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;

	return 0;
}

/**
 * nyx_signal_init() - Install the signal handlers and open the self-pipe.
 *
 * Handlers are installed for SIGINT, SIGTERM and SIGHUP. SA_RESTART is set
 * so that an interrupted system call elsewhere in the program resumes
 * rather than failing with EINTR: poll() still wakes up regardless,
 * because the pipe becomes readable.
 *
 * On failure everything already set up is undone before returning, so the
 * caller does not have to distinguish a partial failure from a total one.
 *
 * Return: A file descriptor that becomes readable when one of the handled
 * signals is delivered, or -1 on failure, including a second init while the
 * module is already initialized. The descriptor is owned by this
 * module and is closed by nyx_signal_finish(); the caller must not close
 * it.
 */
int
nyx_signal_init(void)
{
	struct sigaction sa;
	int fds[2];
	int i;

	if (pipe_read_fd >= 0 || pipe_write_fd >= 0) {
		errno = EALREADY;
		nyx_log("signal module is already initialized");
		return -1;
	}

	if (pipe(fds) < 0) {
		nyx_log("cannot create signal pipe: %s", strerror(errno));
		return -1;
	}

	pipe_read_fd = fds[0];
	pipe_write_fd = fds[1];

	if (set_flags(pipe_read_fd) < 0 || set_flags(pipe_write_fd) < 0) {
		nyx_log("cannot configure signal pipe: %s", strerror(errno));
		nyx_signal_finish();
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;

	saw_terminate = 0;
	saw_reload = 0;

	for (i = 0; i < HANDLED_COUNT; i++) {
		if (sigaction(handled_signals[i], &sa,
		              &previous_action[i]) < 0) {
			nyx_log("cannot install handler for signal %d: %s",
			        handled_signals[i], strerror(errno));
			nyx_signal_finish();
			return -1;
		}
		previous_valid[i] = 1;
	}

	return pipe_read_fd;
}

/**
 * nyx_signal_poll() - Drain pending signals and report what they were.
 * @terminate: set to 1 if a SIGINT or SIGTERM was seen. Must not be NULL.
 * @reload: set to 1 if a SIGHUP was seen. Must not be NULL.
 *
 * The answer comes from the two flags the handler sets, not from the bytes
 * in the pipe. Draining the pipe is what clears its readability so poll()
 * stops firing; the byte values are not consulted, because a byte can be
 * dropped when the pipe is full and a flag cannot. Both output flags are
 * only ever set, never cleared, which lets the caller initialise them once
 * and pass them through several calls.
 *
 * The flags are read and cleared with the handled signals blocked, so a
 * signal delivered inside this function is either counted in this call or
 * left set for the next one -- never lost between the read and the clear.
 *
 * Return: 0 on success, -1 if the pipe was never opened, was closed at the
 * write end, or failed unrecoverably.
 */
int
nyx_signal_poll(int *terminate, int *reload)
{
	unsigned char buf[64];
	sigset_t blocked, previous;
	int i;
	int rc = 0;

	if (pipe_read_fd < 0)
		return -1;

	for (;;) {
		ssize_t n = read(pipe_read_fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			/* EWOULDBLOCK is a synonym for EAGAIN on Linux, and
			 * comparing against both would be a tautology there;
			 * the second test exists for systems where POSIX
			 * allows them to differ. */
			if (errno == EAGAIN)
				break;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
			if (errno == EWOULDBLOCK)
				break;
#endif
			nyx_log("signal pipe read failed: %s", strerror(errno));
			return -1;
		}
		if (n == 0) {
			/* The write end is gone, which this module never does
			 * while running. Report it, but still collect the
			 * flags below: a signal already recorded must not be
			 * thrown away on the way out. */
			rc = -1;
			break;
		}
		if ((size_t)n < sizeof(buf))
			break;
	}

	sigemptyset(&blocked);
	for (i = 0; i < HANDLED_COUNT; i++)
		sigaddset(&blocked, handled_signals[i]);

	if (sigprocmask(SIG_BLOCK, &blocked, &previous) == 0) {
		if (saw_terminate) {
			saw_terminate = 0;
			*terminate = 1;
		}
		if (saw_reload) {
			saw_reload = 0;
			*reload = 1;
		}
		(void)sigprocmask(SIG_SETMASK, &previous, NULL);
	} else {
		/* Without the mask the read-and-clear is not atomic against
		 * delivery, so do not clear: a repeated report is harmless,
		 * a dropped one is not. */
		if (saw_terminate)
			*terminate = 1;
		if (saw_reload)
			*reload = 1;
	}

	return rc;
}

/**
 * nyx_signal_finish() - Restore default dispositions and close the pipe.
 *
 * Safe to call after a failed or partial nyx_signal_init(), and safe to
 * call twice: the descriptors are set to -1 as they are closed and the
 * sigaction() results are deliberately ignored, since there is nothing
 * useful to do about a failure on the way out.
 *
 * What is restored is the disposition that was found, not SIG_DFL. A shell
 * that starts this program under nohup leaves SIGHUP as SIG_IGN, and a
 * process that resets it to SIG_DFL on exit has quietly changed the state
 * its caller set up. Only the signals actually replaced are restored,
 * which is what makes this safe after a partial init.
 *
 * What closes the window against a signal arriving mid-teardown is the
 * order of the two phases, not the order of the two close() calls: once
 * every handled signal is restored, no handler of ours can run and so none
 * can reach pipe_write_fd. A signal delivered *during* the restore loop --
 * after SIGINT has been put back but before SIGHUP has -- still runs the
 * handler, and still finds both descriptors open, which is why the loop
 * comes first and the closes second.
 */
void
nyx_signal_finish(void)
{
	int i;

	for (i = 0; i < HANDLED_COUNT; i++) {
		if (!previous_valid[i])
			continue;
		(void)sigaction(handled_signals[i], &previous_action[i], NULL);
		previous_valid[i] = 0;
	}

	if (pipe_write_fd >= 0) {
		close((int)pipe_write_fd);
		pipe_write_fd = -1;
	}
	if (pipe_read_fd >= 0) {
		close(pipe_read_fd);
		pipe_read_fd = -1;
	}

	saw_terminate = 0;
	saw_reload = 0;
}
