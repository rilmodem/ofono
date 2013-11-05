/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013  Canonical Ltd.
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

#ifndef __GRILREPLY_H
#define __GRILREPLY_H

#include <ofono/types.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct reply_setup_data_call {
	guint version;
	guint status;
	gint cid;
	guint retry_time;
	guint active;
	guint protocol;
	gchar *ifname;
	gchar **dns_addresses;
	gchar **gateways;
	gchar **ip_addrs;
};

struct reply_sim_io {
	int sw1;
	int sw2;
	int hex_len;
	unsigned char *hex_response;
};

#define MAX_UICC_APPS 16

struct reply_sim_app {
	guint app_type;
	guint app_state;
	guint perso_substate;
	char *aid_str;
	char *app_str;
	guint pin_replaced;
	guint pin1_state;
	guint pin2_state;
};

struct reply_sim_status {
	guint card_state;
	guint pin_state;
	guint gsm_umts_index;
	guint cdma_index;
	guint ims_index;
	guint num_apps;
	struct reply_sim_app *apps[MAX_UICC_APPS];
};

void g_ril_reply_free_setup_data_call(struct reply_setup_data_call *reply);

struct reply_setup_data_call *g_ril_reply_parse_data_call(GRil *gril,
							struct ril_msg *message,
							struct ofono_error *error);

void g_ril_reply_free_sim_io(struct reply_sim_io *reply);

struct reply_sim_io *g_ril_reply_parse_sim_io(GRil *gril,
						struct ril_msg *message);

gchar *g_ril_reply_parse_imsi(GRil *gril, struct ril_msg *message);

void g_ril_reply_free_sim_status(struct reply_sim_status *status);

struct reply_sim_status *g_ril_reply_parse_sim_status(GRil *gril,
							struct ril_msg *message);

#ifdef __cplusplus
}
#endif

#endif /* __GRILREPLY_H */
