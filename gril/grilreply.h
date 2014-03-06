/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2014  Canonical Ltd.
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
#include <ofono/sim.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct reply_operator {
	char *lalpha;
	char *salpha;
	char *numeric;
	char *status;
	int tech;
};

struct reply_avail_ops {
	guint num_ops;
	GSList *list;
};

struct reply_reg_state {
	int status;
	int lac;
	int ci;
	int tech;
	int reason;
	unsigned int max_cids;
};

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

struct reply_clir {
	int status;
	int provisioned;
};

void g_ril_reply_free_avail_ops(struct reply_avail_ops *reply);

struct reply_avail_ops *g_ril_reply_parse_avail_ops(GRil *gril,
						const struct ril_msg *message);
void g_ril_reply_free_operator(struct reply_operator *reply);

struct reply_operator *g_ril_reply_parse_operator(GRil *gril,
						const struct ril_msg *message);

void g_ril_reply_free_setup_data_call(struct reply_setup_data_call *reply);

struct reply_setup_data_call *g_ril_reply_parse_data_call(GRil *gril,
						const struct ril_msg *message,
						struct ofono_error *error);

void g_ril_reply_free_sim_io(struct reply_sim_io *reply);

struct reply_sim_io *g_ril_reply_parse_sim_io(GRil *gril,
						const struct ril_msg *message);

gchar *g_ril_reply_parse_imsi(GRil *gril, const struct ril_msg *message);

struct reply_reg_state *g_ril_reply_parse_reg_state(GRil *gril,
						const struct ril_msg *message);

void g_ril_reply_free_sim_status(struct reply_sim_status *status);

struct reply_sim_status *g_ril_reply_parse_sim_status(GRil *gril,
						const struct ril_msg *message);

struct ofono_phone_number *g_ril_reply_parse_get_smsc_address(
						GRil *gril,
						const struct ril_msg *message);

int g_ril_reply_parse_sms_response(GRil *gril, const struct ril_msg *message);

GSList *g_ril_reply_parse_get_calls(GRil *gril, const struct ril_msg *message);

enum ofono_disconnect_reason g_ril_reply_parse_call_fail_cause(
				GRil *gril, const struct ril_msg *message);

int g_ril_reply_parse_get_mute(GRil *gril, const struct ril_msg *message);

char *g_ril_reply_parse_baseband_version(GRil *gril,
						const struct ril_msg *message);

char *g_ril_reply_parse_get_imei(GRil *gril,
					const struct ril_msg *message);

int g_ril_reply_parse_query_call_waiting(GRil *gril,
						const struct ril_msg *message);

int g_ril_reply_parse_query_clip(GRil *gril,
					const struct ril_msg *message);

void g_ril_reply_free_get_clir(struct reply_clir *rclir);

struct reply_clir *g_ril_reply_parse_get_clir(GRil *gril,
						const struct ril_msg *message);

struct ofono_call_forwarding_condition
	*g_ril_reply_parse_query_call_fwd(GRil *gril,
						const struct ril_msg *message,
						unsigned int *list_size);

int g_ril_reply_parse_get_preferred_network_type(GRil *gril,
						const struct ril_msg *message);

int g_ril_reply_parse_query_facility_lock(GRil *gril,
						const struct ril_msg *message);

int g_ril_reply_parse_set_facility_lock(GRil *gril,
					const struct ril_msg *message);

int *g_ril_reply_parse_retries(GRil *gril, const struct ril_msg *message,
				enum ofono_sim_password_type passwd_type);

#ifdef __cplusplus
}
#endif

#endif /* __GRILREPLY_H */
