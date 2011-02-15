/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_EMULATOR_H
#define __OFONO_EMULATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_emulator;
struct ofono_emulator_request;

enum ofono_emulator_type {
	OFONO_EMULATOR_TYPE_DUN,
	OFONO_EMULATOR_TYPE_HFP,
};

enum ofono_emulator_request_type {
	OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY,
	OFONO_EMULATOR_REQUEST_TYPE_QUERY,
	OFONO_EMULATOR_REQUEST_TYPE_SUPPORT,
	OFONO_EMULATOR_REQUEST_TYPE_SET,
};

typedef void (*ofono_emulator_request_cb_t)(struct ofono_emulator *em,
					struct ofono_emulator_request *req,
					void *data);

struct ofono_emulator *ofono_emulator_create(struct ofono_modem *modem,
						enum ofono_emulator_type type);

void ofono_emulator_register(struct ofono_emulator *em, int fd);

void ofono_emulator_remove(struct ofono_emulator *em);

void ofono_emulator_send_final(struct ofono_emulator *em,
				const struct ofono_error *final);
void ofono_emulator_send_unsolicited(struct ofono_emulator *em,
					const char *result);
void ofono_emulator_send_intermediate(struct ofono_emulator *em,
					const char *result);
void ofono_emulator_send_info(struct ofono_emulator *em, const char *line,
				ofono_bool_t last);

ofono_bool_t ofono_emulator_add_handler(struct ofono_emulator *em,
					const char *prefix,
					ofono_emulator_request_cb_t cb,
					void *data, ofono_destroy_func destroy);
ofono_bool_t ofono_emulator_remove_handler(struct ofono_emulator *em,
						const char *prefix);

ofono_bool_t ofono_emulator_request_next_string(
					struct ofono_emulator_request *req,
					const char **str);
ofono_bool_t ofono_emulator_request_next_number(
					struct ofono_emulator_request *req,
					int *number);

const char *ofono_emulator_request_get_raw(struct ofono_emulator_request *req);

enum ofono_emulator_request_type ofono_emulator_request_get_type(
					struct ofono_emulator_request *req);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_EMULATOR_H */
