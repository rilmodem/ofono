/*
 *
 *  oFono - Open Telephony stack for Linux
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

#ifndef __OFONO_HISTORY_H
#define __OFONO_HISTORY_H

#ifdef __cplusplus
extern "C" {
#endif

enum ofono_disconnect_reason;
struct ofono_call;

struct ofono_history_context {
	struct ofono_history_driver *driver;
	struct ofono_modem *modem;
	void *data;
};

struct ofono_history_driver {
	const char *name;
	int (*probe)(struct ofono_history_context *context);
	void (*remove)(struct ofono_history_context *context);
	void (*call_ended)(struct ofono_history_context *context,
				const struct ofono_call *call,
				time_t start, time_t end);
	void (*call_missed)(struct ofono_history_context *context,
				const struct ofono_call *call, time_t when);
};

int ofono_history_driver_register(const struct ofono_history_driver *driver);
void ofono_history_driver_unregister(const struct ofono_history_driver *driver);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_HISTORY_H */
