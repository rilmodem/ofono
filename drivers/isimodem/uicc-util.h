/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  ST-Ericsson AB.
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __ISIMODEM_UICC_UTIL_H
#define __ISIMODEM_UICC_UTIL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <gisi/client.h>

struct uicc_sim_data;

struct uicc_sim_application {
	int id;
	uint8_t type;
	uint8_t status;
	uint8_t length;

	struct uicc_sim_data *sim;
};

struct uicc_sim_data {
	GIsiClient *client;
	unsigned flags;
	int app_id;
	int app_type;
	uint8_t client_id;

	GIsiVersion version;

	gboolean server_running;

	gboolean pin_state_received;
	gboolean passwd_required;

	/* Application state */
	gboolean uicc_app_started;
	uint8_t trying_app_id;
	uint8_t trying_app_type;
	GHashTable *app_table;

	uint8_t pin1_id;
	uint8_t pin2_id;
};

gboolean uicc_get_fileid_path(struct uicc_sim_data *sd,
				int *mf_path,
				int *df1_path,
				int *df2_path,
				unsigned char *df_len,
				int fileid);

uint8_t uicc_get_sfi(const int fileid);

#ifdef __cplusplus
};
#endif

#endif /* __ISIMODEM_UICC_UTIL_H */
