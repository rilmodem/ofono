/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) ST-Ericsson SA 2011.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <gisi/client.h>
#include <gisi/iter.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/ssn.h>

#include "call.h"
#include "isimodem.h"
#include "ss.h"
#include "isiutil.h"
#include "debug.h"

#define NOT_3GGP_NOTIFICATION	-1
#define PN_SS			0x06

/* TS 27.007 Supplementary service notifications +CSSN */
enum ss_cssi {
	SS_MO_UNCONDITIONAL_FORWARDING =	0,
	SS_MO_CONDITIONAL_FORWARDING =		1,
	SS_MO_CALL_FORWARDED =			2,
	SS_MO_CALL_WAITING =			3,
	SS_MO_CUG_CALL =			4,
	SS_MO_OUTGOING_BARRING =		5,
	SS_MO_INCOMING_BARRING =		6,
	SS_MO_CLIR_SUPPRESSION_REJECTED =	7,
	SS_MO_CALL_DEFLECTED =			8,
};

enum ss_cssu {
	SS_MT_CALL_FORWARDED =			0,
	SS_MT_CUG_CALL =			1,
	SS_MT_VOICECALL_ON_HOLD =		2,
	SS_MT_VOICECALL_RETRIEVED =		3,
	SS_MT_MULTIPARTY_VOICECALL =		4,
	SS_MT_VOICECALL_HOLD_RELEASED =		5,
	SS_MT_FORWARD_CHECK_SS_MESSAGE =	6,
	SS_MT_VOICECALL_IN_TRANSFER =		7,
	SS_MT_VOICECALL_TRANSFERRED =		8,
	SS_MT_CALL_DEFLECTED =			9,
};

struct ssn_data {
	GIsiClient *client;
	GIsiClient *primary;
	GIsiClient *secondary;
};

struct isi_ssn_prop {
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 1];
	int type;
	uint16_t cug_index;
};

struct isi_ssn {
	GIsiClient *client;
	struct isi_call_req_context *queue;
};

static void isi_cm_sb_rem_address_sb_proc(struct isi_ssn_prop *ssn_prop,
		GIsiSubBlockIter const *sb)
{
	uint8_t addr_type, addr_len;
	char *address;
	DBG("CALL_SB_REMOTE_ADDRESS");

	if (!g_isi_sb_iter_get_byte(sb, &addr_type, 2) ||
			/* address type */
			/* presentation indicator */
			/* fillerbyte */
			!g_isi_sb_iter_get_byte(sb, &addr_len, 5) ||
			!g_isi_sb_iter_get_alpha_tag(sb, &address, 2 *
					addr_len, 6))
		return;

	strncpy(ssn_prop->number, address, addr_len);

	g_free(address);
}

static void isi_ssn_notify_ofono(void *_ssn, int cssi, int cssu,
		struct isi_ssn_prop *ssn_prop)
{
	struct ofono_phone_number *phone_nr =
			(struct ofono_phone_number *) ssn_prop;

	if (cssi != NOT_3GGP_NOTIFICATION)
		ofono_ssn_cssi_notify(_ssn, cssi, ssn_prop->cug_index);

	if (cssu != NOT_3GGP_NOTIFICATION)
		ofono_ssn_cssu_notify(_ssn, cssi, ssn_prop->cug_index,
				phone_nr);
}

static void isi_ssn_call_modem_sb_notify(GIsiSubBlockIter const *sb)
{
	uint8_t sb_property;
	g_isi_sb_iter_get_byte(sb, &sb_property, 2);

	if (sb_property == CALL_NOTIFY_USER_SUSPENDED)
		DBG("CALL_NOTIFY_USER_SUSPENDED");

	if (sb_property == CALL_NOTIFY_USER_RESUMED)
		DBG("CALL_NOTIFY_USER_RESUMED");

	if (sb_property == CALL_NOTIFY_BEARER_CHANGE)
		DBG("CALL_NOTIFY_BEARER_CHANGE");
}

static void isi_ssn_call_modem_sb_ss_code(GIsiSubBlockIter const *sb,
		int *cssi_p, int *cssu_p)
{
	uint16_t sb_property;
	g_isi_sb_iter_get_word(sb, &sb_property, 2);

	switch (sb_property) {
	case(CALL_SSC_ALL_FWDS):
		DBG("Call forwarding is active");
	break;
	case(CALL_SSC_ALL_COND_FWD): {
		*(cssi_p) = SS_MO_CONDITIONAL_FORWARDING;
		DBG("Some of conditional call forwardings active");
	}
	break;
	case(CALL_SSC_CFU): {
		*(cssi_p) = SS_MO_UNCONDITIONAL_FORWARDING;
		DBG("Unconditional call forwarding is active");
	}
	break;
	case(CALL_SSC_CFB):
		DBG("Unknown notification #1");
	break;
	case(CALL_SSC_CFNRY):
		DBG("Unknown notification #2");
	break;
	case(CALL_SSC_CFGNC):
		DBG("Unknown notification #3");
	break;
	case(CALL_SSC_OUTGOING_BARR_SERV): {
		*(cssi_p) = SS_MO_OUTGOING_BARRING;
		DBG("Outgoing calls are barred");
	}
	break;
	case(CALL_SSC_INCOMING_BARR_SERV): {
		*(cssi_p) = SS_MO_INCOMING_BARRING;
		DBG("Incoming calls are barred");
	}
	break;
	case(CALL_SSC_CALL_WAITING):
		DBG("Incoming calls are barred");
	break;
	case(CALL_SSC_CLIR):
		DBG("CLIR connected unknown indication.");
	break;
	case(CALL_SSC_ETC):
		DBG("Unknown notification #4");
	break;
	case(CALL_SSC_MPTY): {
		*(cssu_p) = SS_MT_MULTIPARTY_VOICECALL;
		DBG("Multiparty call entered.");
	}
	break;
	case(CALL_SSC_CALL_HOLD): {
		*(cssu_p) = SS_MT_VOICECALL_HOLD_RELEASED;
		DBG("Call on hold has been released.");
	}
	break;
	default:
		break;
	}
}

static void isi_ssn_call_modem_sb_ss_status(GIsiSubBlockIter const *sb)
{
	uint8_t sb_property;
	g_isi_sb_iter_get_byte(sb, &sb_property, 2);

	if (sb_property & CALL_SS_STATUS_ACTIVE)
		DBG("CALL_SS_STATUS_ACTIVE");

	if (sb_property & CALL_SS_STATUS_REGISTERED)
		DBG("CALL_SS_STATUS_REGISTERED");

	if (sb_property & CALL_SS_STATUS_PROVISIONED)
		DBG("CALL_SS_STATUS_PROVISIONED");

	if (sb_property & CALL_SS_STATUS_QUIESCENT)
		DBG("CALL_SS_STATUS_QUIESCENT");
}

static void isi_ssn_call_modem_sb_ss_notify(GIsiSubBlockIter const *sb,
		int *cssi_p, int *cssu_p)
{
	uint8_t sb_property;
	g_isi_sb_iter_get_byte(sb, &sb_property, 2);

	if (sb_property & CALL_SSN_INCOMING_IS_FWD) {
		*(cssu_p) = SS_MT_CALL_FORWARDED;
		DBG("This is a forwarded call #1.");
	}

	if (sb_property & CALL_SSN_INCOMING_FWD)
		DBG("This is a forwarded call #2.");

	if (sb_property & CALL_SSN_OUTGOING_FWD) {
		*(cssi_p) = SS_MO_CALL_FORWARDED;
		DBG("Call has been forwarded.");
	}
}

static void isi_ssn_call_modem_sb_ss_notify_ind(GIsiSubBlockIter const *sb,
		int *cssi_p)
{
	uint8_t sb_property;
	g_isi_sb_iter_get_byte(sb, &sb_property, 2);

	if (sb_property & CALL_SSI_CALL_IS_WAITING) {
		*(cssi_p) = SS_MO_CALL_WAITING;
		DBG("Call is waiting.");
	}

	if (sb_property & CALL_SSI_MPTY)
		DBG("Multiparty call.");

	if (sb_property & CALL_SSI_CLIR_SUPPR_REJ) {
		*(cssi_p) = SS_MO_CLIR_SUPPRESSION_REJECTED;
		DBG("CLIR suppression rejected.");
	}
}

static void isi_ssn_call_modem_sb_ss_hold(GIsiSubBlockIter const *sb,
		int *cssu_p)
{
	uint8_t sb_property;
	g_isi_sb_iter_get_byte(sb, &sb_property, 2);

	if (sb_property & CALL_HOLD_IND_RETRIEVED) {
		*(cssu_p) = SS_MT_VOICECALL_RETRIEVED;
		DBG("Call has been retrieved.");
	}

	if (sb_property & CALL_HOLD_IND_ON_HOLD) {
		*(cssu_p) = SS_MT_VOICECALL_ON_HOLD;
		DBG("Call has been put on hold.");
	}
}

static void isi_ssn_call_modem_sb_ss_ect_ind(GIsiSubBlockIter const *sb,
		int *cssu_p)
{
	uint8_t sb_property;
	g_isi_sb_iter_get_byte(sb, &sb_property, 2);

	if (sb_property & CALL_ECT_CALL_STATE_ALERT) {
		*(cssu_p) = SS_MT_VOICECALL_IN_TRANSFER;
		DBG("Call is being connected with the remote party");
		DBG("in alerting state.");
	}

	if (sb_property & CALL_ECT_CALL_STATE_ACTIVE) {
		*(cssu_p) = SS_MT_VOICECALL_TRANSFERRED;
		DBG("Call has been connected with the other remote");
		DBG("party in explicit call transfer operation.");
	}
}

static int isi_ssn_call_modem_sb_cug_info(GIsiSubBlockIter const *sb,
		struct isi_ssn_prop *ssn_prop)
{
	uint8_t sb_property;
	g_isi_sb_iter_get_byte(sb, &sb_property, 2);
	DBG("CALL_SB_CUG_INFO: This is a CUG Call.");
	DBG("Preferential CUG: 0x%x,", sb_property);
	g_isi_sb_iter_get_byte(sb, &sb_property, 3);
	DBG("Cug Output Access: 0x%x,", sb_property);
	g_isi_sb_iter_get_word(sb, &ssn_prop->cug_index, 4);
	DBG("Cug Call Index: 0x%x,", ssn_prop->cug_index);
	return SS_MO_CUG_CALL;
}


static void isi_callmodem_notif_ind_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_ssn *ssn = data;
	struct isi_ssn *issn = ofono_ssn_get_data(ssn);
	struct isi_ssn_prop *ssn_prop = g_try_new0(struct isi_ssn_prop, 1);
	struct {
		uint8_t message_id, call_id, sub_blocks;
	} const *m = data;
	int cssi = NOT_3GGP_NOTIFICATION;
	int cssu = NOT_3GGP_NOTIFICATION;
	GIsiSubBlockIter sb[1];
	DBG("Received CallServer notification.");

	if (g_isi_msg_data_len(msg) < 3)
		goto out;

	if (issn->client == NULL)
		goto out;

	for (g_isi_sb_iter_init(sb, data, (sizeof *m));
			g_isi_sb_iter_is_valid(sb);
			g_isi_sb_iter_next(sb)) {
		switch (g_isi_sb_iter_get_id(sb)) {
		case CALL_SB_NOTIFY:
			isi_ssn_call_modem_sb_notify(sb);
			break;
		case CALL_SB_SS_CODE:
			isi_ssn_call_modem_sb_ss_code(sb, &cssi, &cssu);
			break;
		case CALL_SB_SS_STATUS:
			isi_ssn_call_modem_sb_ss_status(sb);
			break;
		case CALL_SB_SS_NOTIFY:
			isi_ssn_call_modem_sb_ss_notify(sb, &cssi, &cssu);
			break;
		case CALL_SB_SS_NOTIFY_INDICATOR:
			isi_ssn_call_modem_sb_ss_notify_ind(sb, &cssi);
			break;
		case CALL_SB_SS_HOLD_INDICATOR:
			isi_ssn_call_modem_sb_ss_hold(sb, &cssu);
			break;
		case CALL_SB_SS_ECT_INDICATOR:
			isi_ssn_call_modem_sb_ss_ect_ind(sb, &cssu);
			break;
		case CALL_SB_REMOTE_ADDRESS:
			isi_cm_sb_rem_address_sb_proc(ssn_prop, sb);
			break;
		case CALL_SB_REMOTE_SUBADDRESS:
			break;
		case CALL_SB_CUG_INFO:
			cssu = isi_ssn_call_modem_sb_cug_info(sb, ssn_prop);
			break;
		case CALL_SB_ORIGIN_INFO:
			break;
		case CALL_SB_ALERTING_PATTERN:
			break;
		case CALL_SB_ALERTING_INFO:
			break;
		}
	}

	isi_ssn_notify_ofono(ssn, cssi, cssu, ssn_prop);
out:
	g_free(ssn_prop);
}

static gboolean isi_ssn_register(gpointer user)
{
	struct ofono_ssn *ssn = user;
	struct ssn_data *sd = ofono_ssn_get_data(ssn);
	DBG("");

	g_isi_client_ind_subscribe(sd->client, CALL_GSM_NOTIFICATION_IND,
			isi_callmodem_notif_ind_cb, ssn);

	ofono_ssn_register(user);

	return FALSE;
}

static void ssn_primary_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_ssn *ssn = data;
	struct ssn_data *sd = ofono_ssn_get_data(ssn);

	if (g_isi_msg_error(msg) < 0)
		return;

	if (sd == NULL)
		return;
	sd->client = sd->primary;
	g_isi_client_destroy(sd->secondary);

	ISI_VERSION_DBG(msg);

	g_idle_add(isi_ssn_register, ssn);
}

static void ssn_secondary_reachable_cb(const GIsiMessage *msg, void *data)
{
	struct ofono_ssn *ssn = data;
	struct ssn_data *sd = ofono_ssn_get_data(ssn);

	if (g_isi_msg_error(msg) < 0)
		return;

	if (sd == NULL)
		return;

	sd->client = sd->secondary;
	g_isi_client_destroy(sd->primary);

	ISI_VERSION_DBG(msg);
}

static int isi_ssn_probe(struct ofono_ssn *ssn, unsigned int vendor,
		void *user)
{
	GIsiModem *modem = user;
	struct ssn_data *sd;

	sd = g_try_new0(struct ssn_data, 1);
	if (sd == NULL)
		return -ENOMEM;

	sd->primary = g_isi_client_create(modem, PN_MODEM_CALL);

	if (sd->primary == NULL) {
		g_free(sd);
		return -ENOMEM;
	}

	sd->secondary = g_isi_client_create(modem, PN_SS);

	if (sd->client == NULL) {
		g_free(sd);
		return -ENOMEM;
	}

	g_isi_client_verify(sd->primary, ssn_primary_reachable_cb, ssn, NULL);
	g_isi_client_verify(sd->secondary, ssn_secondary_reachable_cb, ssn,
				NULL);

	ofono_ssn_set_data(ssn, sd);

	return 0;
}

static void isi_ssn_remove(struct ofono_ssn *ssn)
{
	struct ssn_data *data = ofono_ssn_get_data(ssn);

	ofono_ssn_set_data(ssn, NULL);

	if (data == NULL)
		return;

	g_isi_client_destroy(data->client);
	g_free(data);
}

static struct ofono_ssn_driver driver = {
		.name			= "isimodem",
		.probe			= isi_ssn_probe,
		.remove			= isi_ssn_remove
};

void isi_ssn_init(void)
{
	ofono_ssn_driver_register(&driver);
}

void isi_ssn_exit(void)
{
	ofono_ssn_driver_unregister(&driver);
}
