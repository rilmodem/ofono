/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/voicecall.h>

#include "gatchat.h"
#include "gatresult.h"

#include "calypsomodem.h"

static const char *none_prefix[] = { NULL };

struct voicecall_data {
	GAtChat *chat;
};

static void calypso_generic_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_voicecall_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void calypso_template(struct ofono_voicecall *vc, const char *cmd,
				ofono_voicecall_cb_t cb, void *data)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	struct cb_data *cbd = cb_data_new(cb, data);

	if (g_at_chat_send(vd->chat, cmd, none_prefix,
				calypso_generic_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void calypso_dial(struct ofono_voicecall *vc,
				const struct ofono_phone_number *ph,
				enum ofono_clir_option clir,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[256];

	if (ph->type == 145)
		snprintf(buf, sizeof(buf), "ATD+%s", ph->number);
	else
		snprintf(buf, sizeof(buf), "ATD%s", ph->number);

	switch (clir) {
	case OFONO_CLIR_OPTION_INVOCATION:
		strcat(buf, "I");
		break;
	case OFONO_CLIR_OPTION_SUPPRESSION:
		strcat(buf, "i");
		break;
	default:
		break;
	}

	strcat(buf, ";");

	calypso_template(vc, buf, cb, data);
}

static void calypso_answer(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "ATA", cb, data);
}

static void calypso_ath(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "ATH", cb, data);
}

static void calypso_chup(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "AT+CHUP", cb, data);
}

static void calypso_hold_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "AT+CHLD=2", cb, data);
}

static void calypso_release_all_held(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "AT+CHLD=0", cb, data);
}

static void calypso_set_udub(struct ofono_voicecall *vc,
				ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "AT+CHLD=0", cb, data);
}

static void calypso_release_all_active(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "AT+CHLD=1", cb, data);
}

static void calypso_release_specific(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[32];

	/* On calypso, 1X only releases active calls, while 7X releases
	 * active or held calls
	 */
	snprintf(buf, sizeof(buf), "AT%%CHLD=7%d", id);
	calypso_template(vc, buf, cb, data);
}

static void calypso_private_chat(struct ofono_voicecall *vc, int id,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[32];

	snprintf(buf, sizeof(buf), "AT+CHLD=2%d", id);
	calypso_template(vc, buf, cb, data);
}

static void calypso_create_multiparty(struct ofono_voicecall *vc,
					ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "AT+CHLD=3", cb, data);
}

static void calypso_transfer(struct ofono_voicecall *vc,
			ofono_voicecall_cb_t cb, void *data)
{
	calypso_template(vc, "AT+CHLD=4", cb, data);
}

static void calypso_deflect(struct ofono_voicecall *vc,
				const struct ofono_phone_number *ph,
				ofono_voicecall_cb_t cb, void *data)
{
	char buf[128];

	snprintf(buf, sizeof(buf), "AT+CTFR=%s,%d", ph->number, ph->type);
	calypso_template(vc, buf, cb, data);
}

static void calypso_send_dtmf(struct ofono_voicecall *vc, const char *dtmf,
			ofono_voicecall_cb_t cb, void *data)
{
	int len = strlen(dtmf);
	int s;
	int i;
	char *buf;

	/* strlen("+VTS=\"T\";") = 9 + initial AT + null */
	buf = g_try_new(char, len * 9 + 3);

	if (buf == NULL) {
		CALLBACK_WITH_FAILURE(cb, data);
		return;
	}

	s = sprintf(buf, "AT+VTS=%c", dtmf[0]);

	for (i = 1; i < len; i++)
		s += sprintf(buf + s, ";+VTS=%c", dtmf[i]);

	calypso_template(vc, buf, cb, data);
	g_free(buf);
}

static void cpi_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);
	GAtResultIter iter;
	int id;
	int msgtype;
	int direction;
	int mode;
	const char *num;
	int type;
	int cause;
	int line = 0;
	int validity;
	struct ofono_call call;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "%CPI:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &id))
		return;

	/* msgtype
	 * 0 - setup
	 * 1 - disconnect
	 * 2 - alert
	 * 3 - call proceed
	 * 4 - sync
	 * 5 - progress
	 * 6 - connected
	 * 7 - release
	 * 8 - reject
	 * 9 - request (MO Setup)
	 * 10 - hold
	 */
	if (!g_at_result_iter_next_number(&iter, &msgtype))
		return;

	/* Skip in-band ring tone notification */
	if (!g_at_result_iter_skip_next(&iter))
		return;

	/* Skip traffic channel assignment */
	if (!g_at_result_iter_skip_next(&iter))
		return;

	if (!g_at_result_iter_next_number(&iter, &direction))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	DBG("id:%d, msgtype:%d, direction:%d, mode:%d",
		id, msgtype, direction, mode);

	if (!g_at_result_iter_next_string(&iter, &num))
		return;

	if (strlen(num) > 0) {
		DBG("Len > 0");
		validity = 0;

		if (!g_at_result_iter_next_number(&iter, &type))
			return;

		DBG("type obtained");
	} else {
		DBG("skip next");
		validity = 2;
		type = 129;

		if (!g_at_result_iter_skip_next(&iter))
			return;
		DBG("skipped");
	}

	DBG("num:%s, type:%d", num, type);

	/* Skip alpha field */
	if (!g_at_result_iter_skip_next(&iter))
		return;

	g_at_result_iter_next_number(&iter, &cause);
	g_at_result_iter_next_number(&iter, &line);

	DBG("cause:%d, line:%d", cause, line);

	/* We only care about voice calls here */
	if (mode != 0)
		return;

	if (line != 0) {
		ofono_error("Alternate Line service not yet handled");
		return;
	}

	/* Need to send this on the calypso hardware to avoid echo issues */
	if (msgtype == 3 || msgtype == 4)
		g_at_chat_send(vd->chat, "AT%N0187", none_prefix,
				NULL, NULL, NULL);

	ofono_call_init(&call);

	switch (msgtype) {
	case 0:
		/* Set call status to incoming */
		call.status = 4;
		break;
	case 2:
		/* Set call status to alerting */
		call.status = 3;
		break;
	case 3:
	case 9:
		/* Set call status to dialing */
		call.status = 2;
		break;
	case 6:
		/* Set call status to connected */
		call.status = 0;
		break;
	case 10:
		/* Set call status to held */
		call.status = 1;
		break;
	case 1:
	case 8:
		ofono_voicecall_disconnected(vc, id,
					OFONO_DISCONNECT_REASON_UNKNOWN, NULL);
		return;
	default:
		return;
	};

	call.id = id;
	call.type = mode;
	call.direction = direction;
	strncpy(call.phone_number.number, num,
		OFONO_MAX_PHONE_NUMBER_LENGTH);
	call.phone_number.number[OFONO_MAX_PHONE_NUMBER_LENGTH] = '\0';
	call.phone_number.type = type;
	call.clip_validity = validity;

	ofono_voicecall_notify(vc, &call);
}

static void calypso_voicecall_initialized(gboolean ok, GAtResult *result,
					gpointer user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	DBG("voicecall_init: registering to notifications");

	g_at_chat_register(vd->chat, "%CPI:", cpi_notify, FALSE, vc, NULL);

	ofono_voicecall_register(vc);
}

static int calypso_voicecall_probe(struct ofono_voicecall *vc,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct voicecall_data *vd;

	vd = g_try_new0(struct voicecall_data, 1);
	if (vd == NULL)
		return -ENOMEM;

	vd->chat = g_at_chat_clone(chat);

	ofono_voicecall_set_data(vc, vd);

	g_at_chat_send(vd->chat, "AT%CPI=3", NULL,
				calypso_voicecall_initialized, vc, NULL);

	return 0;
}

static void calypso_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *vd = ofono_voicecall_get_data(vc);

	ofono_voicecall_set_data(vc, NULL);

	g_at_chat_unref(vd->chat);
	g_free(vd);
}

static struct ofono_voicecall_driver driver = {
	.name			= "calypsomodem",
	.probe			= calypso_voicecall_probe,
	.remove			= calypso_voicecall_remove,
	.dial			= calypso_dial,
	.answer			= calypso_answer,
	.hangup_all		= calypso_ath,
	.hangup_active		= calypso_chup,
	.hold_all_active	= calypso_hold_all_active,
	.release_all_held	= calypso_release_all_held,
	.set_udub		= calypso_set_udub,
	.release_all_active	= calypso_release_all_active,
	.release_specific	= calypso_release_specific,
	.private_chat		= calypso_private_chat,
	.create_multiparty	= calypso_create_multiparty,
	.transfer		= calypso_transfer,
	.deflect		= calypso_deflect,
	.swap_without_accept	= NULL,
	.send_tones		= calypso_send_dtmf
};

void calypso_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void calypso_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
