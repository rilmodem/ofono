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

#ifndef __OFONO_STK_H
#define __OFONO_STK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_stk;

typedef void (*ofono_stk_envelope_cb_t)(const struct ofono_error *error,
					const unsigned char *rdata,
					int length, void *data);

typedef void (*ofono_stk_generic_cb_t)(const struct ofono_error *error,
					void *data);

struct ofono_stk_driver {
	const char *name;
	int (*probe)(struct ofono_stk *stk, unsigned int vendor, void *data);
	void (*remove)(struct ofono_stk *stk);
	void (*envelope)(struct ofono_stk *stk,
				int length, const unsigned char *command,
				ofono_stk_envelope_cb_t cb, void *data);
	void (*terminal_response)(struct ofono_stk *stk,
					int length, const unsigned char *resp,
					ofono_stk_generic_cb_t cb, void *data);
	void (*user_confirmation)(struct ofono_stk *stk, ofono_bool_t confirm);
};

int ofono_stk_driver_register(const struct ofono_stk_driver *d);
void ofono_stk_driver_unregister(const struct ofono_stk_driver *d);

struct ofono_stk *ofono_stk_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_stk_register(struct ofono_stk *stk);
void ofono_stk_remove(struct ofono_stk *stk);

void ofono_stk_set_data(struct ofono_stk *stk, void *data);
void *ofono_stk_get_data(struct ofono_stk *stk);

void ofono_stk_proactive_command_notify(struct ofono_stk *stk,
					int length, const unsigned char *pdu);

void ofono_stk_proactive_session_end_notify(struct ofono_stk *stk);

void ofono_stk_proactive_command_handled_notify(struct ofono_stk *stk,
						int length,
						const unsigned char *pdu);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_STK_H */
