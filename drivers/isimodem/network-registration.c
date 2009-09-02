/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Aki Niemi <aki.niemi@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>

#include <glib.h>

#include <gisi/netlink.h>
#include <gisi/client.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "isi.h"

#define PN_NETWORK		0x0A
#define NETWORK_TIMEOUT		5
#define NETWORK_SCAN_TIMEOUT	60

/* Used with COMMON_MESSAGE */
enum sub_message_id {
	COMM_ISI_VERSION_GET_REQ = 0x12,
	COMM_ISI_VERSION_GET_RESP = 0x13,
	COMM_ISA_ENTITY_NOT_REACHABLE_RESP = 0x14
};

enum message_id {
	NET_SET_REQ = 0x07,
	NET_SET_RESP = 0x08,
	NET_SET_CANCEL_REQ = 0x09,
	NET_SET_CANCEL_RESP = 0x0A,
	NET_RSSI_GET_REQ = 0x0B,
	NET_RSSI_GET_RESP = 0x0C,
	NET_RSSI_IND = 0x1E,
	NET_NITZ_NAME_IND = 0x43,
	NET_REG_STATUS_GET_REQ = 0xE0,
	NET_REG_STATUS_GET_RESP = 0xE1,
	NET_REG_STATUS_IND = 0xE2,
	NET_AVAILABLE_GET_REQ = 0xE3,
	NET_AVAILABLE_GET_RESP = 0xE4,
	NET_OPER_NAME_GET_REQ = 0xE5,
	NET_OPER_NAME_GET_RESP = 0xE6,
	COMMON_MESSAGE = 0xF0
};

enum sub_block_id {
	NET_REG_INFO_COMMON = 0x00,
	NET_RSSI_CURRENT = 0x04,
 	NET_GSM_REG_INFO = 0x09,
	NET_GSM_BAND_INFO = 0x11
};

enum reg_status {
	NET_REG_STATUS_HOME = 0x00,
	NET_REG_STATUS_ROAM = 0x01,
	NET_REG_STATUS_NOSERV = 0x03,
	NET_REG_STATUS_NOSERV_SEARCHING = 0x04,
	NET_REG_STATUS_NOSERV_NOTSEARCHING = 0x05,
	NET_REG_STATUS_NOSERV_NOSIM = 0x06,
	NET_REG_STATUS_POWER_OFF = 0x08,
	NET_REG_STATUS_NSPS = 0x09,
	NET_REG_STATUS_NSPS_NO_COVERAGE = 0x0A,
	NET_REG_STATUS_NOSERV_SIM_REJECTED_BY_NW = 0x0B
};

enum cs_type {
	NET_CS_GSM = 0x00
};

enum measurement_type {
	NET_CURRENT_CELL_RSSI = 0x02
};

enum search_mode {
	NET_MANUAL_SEARCH = 0x00
};

enum band_info {
	NET_GSM_BAND_ALL_SUPPORTED_BANDS = 0x03
};

enum select_mode {
	NET_SELECT_MODE_UNKNOWN = 0x00,
	NET_SELECT_MODE_MANUAL = 0x01,
	NET_SELECT_MODE_AUTOMATIC = 0x02
};

enum return_code {
	NET_CAUSE_OK = 0x00,
	NET_CAUSE_COMMUNICATION_ERROR = 0x01,
	NET_CAUSE_NET_NOT_FOUND = 0x05,
	NET_CAUSE_NO_SELECTED_NETWORK = 0x11
};

struct netreg_data {
	GIsiClient *client;
	struct isi_version version;
};

static void isi_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data)
{
}

static void isi_current_operator(struct ofono_netreg *netreg,
					ofono_netreg_operator_cb_t cb,
					void *data)
{
}

static void isi_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb,
				void *data)
{
}

static void isi_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb,
				void *data)
{
}

static void isi_register_manual(struct ofono_netreg *netreg,
				const struct ofono_network_operator *oper,
				ofono_netreg_register_cb_t cb, void *data)
{
}

static void isi_deregister(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb,
				void *data)
{
}

static void isi_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb,
				void *data)
{
}

static gboolean isi_netreg_register(gpointer user)
{
	struct ofono_netreg *net = user;

	ofono_netreg_register(net);

	return FALSE;
}

static int isi_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *user)
{
	GIsiModem *idx = user;
	struct netreg_data *data = g_try_new0(struct netreg_data, 1);

	if (!data)
		return -ENOMEM;

	data->client = g_isi_client_create(idx, PN_NETWORK);
	if (!data->client) {
		g_free(data);
		return -ENOMEM;
	}

	ofono_netreg_set_data(netreg, data);

	g_idle_add(isi_netreg_register, netreg);

	return 0;
}

static void isi_netreg_remove(struct ofono_netreg *net)
{
	struct netreg_data *data = ofono_netreg_get_data(net);

	if (data && data->client) {
		g_isi_client_destroy(data->client);
		g_free(data);
	}
}

static struct ofono_netreg_driver driver = {
	.name			= "isimodem",
	.probe			= isi_netreg_probe,
	.remove			= isi_netreg_remove,
	.registration_status 	= isi_registration_status,
	.current_operator 	= isi_current_operator,
	.list_operators		= isi_list_operators,
	.register_auto		= isi_register_auto,
	.register_manual	= isi_register_manual,
	.deregister		= isi_deregister,
	.strength		= isi_strength,
};

void isi_netreg_init()
{
	ofono_netreg_driver_register(&driver);
}

void isi_netreg_exit()
{
	ofono_netreg_driver_unregister(&driver);
}
