/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include <stdarg.h>
#include <syslog.h>

#include "ofono.h"

static volatile gboolean debug_enabled = FALSE;

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

	if (debug_enabled == FALSE)
		return;

	va_start(ap, format);

	vsyslog(LOG_DEBUG, format, ap);

	va_end(ap);
}

void __ofono_toggle_debug(void)
{
	if (debug_enabled == TRUE)
		debug_enabled = FALSE;
	else
		debug_enabled = TRUE;
}

int __ofono_log_init(gboolean detach, gboolean debug)
{
	int option = LOG_NDELAY | LOG_PID;

	if (detach == FALSE)
		option |= LOG_PERROR;

	openlog("ofonod", option, LOG_DAEMON);

	syslog(LOG_INFO, "oFono version %s", VERSION);

	debug_enabled = debug;

	return 0;
}

void __ofono_log_cleanup(void)
{
	syslog(LOG_INFO, "Exit");

	closelog();
}
