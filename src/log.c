/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdarg.h>
#include <syslog.h>
#include <stdlib.h>
#include <execinfo.h>
#include <dlfcn.h>

#include "ofono.h"

/**
 * ofono_info:
 * @format: format string
 * @Varargs: list of arguments
 *
 * Output general information
 */
void ofono_info(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	vsyslog(LOG_INFO, format, ap);

	va_end(ap);
}

/**
 * ofono_warn:
 * @format: format string
 * @Varargs: list of arguments
 *
 * Output warning messages
 */
void ofono_warn(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	vsyslog(LOG_WARNING, format, ap);

	va_end(ap);
}

/**
 * ofono_error:
 * @format: format string
 * @varargs: list of arguments
 *
 * Output error messages
 */
void ofono_error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	vsyslog(LOG_ERR, format, ap);

	va_end(ap);
}

/**
 * ofono_debug:
 * @format: format string
 * @varargs: list of arguments
 *
 * Output debug message
 *
 * The actual output of the debug message is controlled via a command line
 * switch. If not enabled, these messages will be ignored.
 */
void ofono_debug(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);

	vsyslog(LOG_DEBUG, format, ap);

	va_end(ap);
}

static void signal_handler(int signo)
{
	void *frames[64];
	char **symbols;
	size_t n_ptrs;
	unsigned int i;

	n_ptrs = backtrace(frames, G_N_ELEMENTS(frames));
	symbols = backtrace_symbols(frames, n_ptrs);
	if (symbols == NULL) {
		ofono_error("No backtrace symbols");
		exit(1);
	}

	ofono_error("Aborting (signal %d)", signo);
	ofono_error("++++++++ backtrace ++++++++");

	for (i = 1; i < n_ptrs; i++)
		ofono_error("[%d]: %s", i - 1, symbols[i]);

	ofono_error("+++++++++++++++++++++++++++");

	g_free(symbols);
	exit(1);
}

static void signal_setup(sighandler_t handler)
{
	struct sigaction sa;
	sigset_t mask;

	sigemptyset(&mask);
	sa.sa_handler = handler;
	sa.sa_mask = mask;
	sa.sa_flags = 0;
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGFPE, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);
}

extern struct ofono_debug_desc __start___debug[];
extern struct ofono_debug_desc __stop___debug[];

static gchar **enabled = NULL;

static ofono_bool_t is_enabled(struct ofono_debug_desc *desc)
{
	int i;

	if (enabled == NULL)
		return FALSE;

	for (i = 0; enabled[i] != NULL; i++) {
		if (desc->name != NULL && g_pattern_match_simple(enabled[i],
							desc->name) == TRUE)
			return TRUE;
		if (desc->file != NULL && g_pattern_match_simple(enabled[i],
							desc->file) == TRUE)
			return TRUE;
	}

	return FALSE;
}

int __ofono_log_init(const char *debug, ofono_bool_t detach)
{
	int option = LOG_NDELAY | LOG_PID;
	struct ofono_debug_desc *desc;
	const char *name = NULL, *file = NULL;

	if (debug != NULL)
		enabled = g_strsplit_set(debug, ":, ", 0);

	for (desc = __start___debug; desc < __stop___debug; desc++) {
		if (file != NULL || name != NULL) {
			if (g_strcmp0(desc->file, file) == 0) {
				if (desc->name == NULL)
					desc->name = name;
			} else
				file = NULL;
		}

		if (is_enabled(desc) == TRUE)
			desc->flags |= OFONO_DEBUG_FLAG_PRINT;
	}

	if (detach == FALSE)
		option |= LOG_PERROR;

	signal_setup(signal_handler);

	openlog("ofonod", option, LOG_DAEMON);

	syslog(LOG_INFO, "oFono version %s", VERSION);

	return 0;
}

void __ofono_log_cleanup(void)
{
	syslog(LOG_INFO, "Exit");

	closelog();

	signal_setup(SIG_DFL);

	g_strfreev(enabled);
}
