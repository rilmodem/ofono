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

#ifndef __GRILREQUEST_H
#define __GRILREQUEST_H

#include <ofono/types.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct req_deactivate_data_call {
	gint cid;
	guint reason;
};

struct req_setup_data_call {
	guint tech;
	guint data_profile;
	gchar *apn;
	gchar *username;
	gchar *password;
	guint auth_type;
	guint protocol;
};

struct req_sim_read_info {
	guint app_type;
	gchar *aid_str;
	int fileid;
	const unsigned char *path;
	unsigned int path_len;
};


struct req_sim_read_binary {
	guint app_type;
	gchar *aid_str;
	int fileid;
	const unsigned char *path;
	unsigned int path_len;
	int start;
	int length;
};


struct req_sim_read_record {
	guint app_type;
	gchar *aid_str;
	int fileid;
	const unsigned char *path;
	unsigned int path_len;
	int record;
	int length;
};


struct req_pin_change_state {
	const gchar *aid_str;
	enum ofono_sim_password_type passwd_type;
	int enable;
	const char *passwd;
};

gboolean g_ril_request_deactivate_data_call(GRil *gril,
				const struct req_deactivate_data_call *req,
				struct parcel *rilp,
				struct ofono_error *error);

void g_ril_request_power(GRil *gril,
				gboolean power,
				struct parcel *rilp);

void g_ril_request_set_net_select_manual(GRil *gril,
					const char *mccmnc,
					struct parcel *rilp);

gboolean g_ril_request_setup_data_call(GRil *gril,
					const struct req_setup_data_call *req,
					struct parcel *rilp,
					struct ofono_error *error);

gboolean g_ril_request_sim_read_info(GRil *gril,
					const struct req_sim_read_info *req,
					struct parcel *rilp);

gboolean g_ril_request_sim_read_binary(GRil *gril,
					const struct req_sim_read_binary *req,
					struct parcel *rilp);

gboolean g_ril_request_sim_read_record(GRil *gril,
					const struct req_sim_read_record *req,
					struct parcel *rilp);

void g_ril_request_read_imsi(GRil *gril,
				const gchar *aid_str,
				struct parcel *rilp);

void g_ril_request_pin_send(GRil *gril,
				const char *passwd,
				const gchar *aid_str,
				struct parcel *rilp);

gboolean g_ril_request_pin_change_state(GRil *gril,
					const struct req_pin_change_state *req,
					struct parcel *rilp);

void g_ril_request_pin_send_puk(GRil *gril,
				const char *puk,
				const char *passwd,
				const gchar *aid_str,
				struct parcel *rilp);

void g_ril_request_change_passwd(GRil *gril,
					const char *old_passwd,
					const char *new_passwd,
					const gchar *aid_str,
					struct parcel *rilp);

#ifdef __cplusplus
}
#endif

#endif /* __GRILREQUEST_H */
