/*
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
 *  Copyright (C) 2009  Collabora Ltd. All rights reserved.
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
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/devinfo.h>
#include <ofono/message-waiting.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/ssn.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>

#include <drivers/hfpmodem/hfpmodem.h>

static const char *brsf_prefix[] = { "+BRSF:", NULL };
static const char *cind_prefix[] = { "+CIND:", NULL };
static const char *cmer_prefix[] = { "+CMER:", NULL };

static int hfp_disable(struct ofono_modem *modem);

static void hfp_debug(const char *str, void *user_data)
{
	ofono_info("%s", str);
}

static void cind_status_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int index;
	int value;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CIND:"))
		goto error;

	index = 1;

	while (g_at_result_iter_next_number(&iter, &value)) {
		int i;

		for (i = 0; i < HFP_INDICATOR_LAST; i++) {
			if (index != data->cind_pos[i])
				continue;

			data->cind_val[i] = value;
		}

		index += 1;
	}

	ofono_info("Service level connection established");
	g_at_chat_send(data->chat, "AT+CMEE=1", NULL, NULL, NULL, NULL);

	ofono_modem_set_powered(modem, TRUE);
	return;

error:
	hfp_disable(modem);
}

static void cmer_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);

	if (!ok) {
		hfp_disable(modem);
		return;
	}

	g_at_chat_send(data->chat, "AT+CIND?", cind_prefix,
			cind_status_cb, modem, NULL);
}

static void cind_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	const char *str;
	int index;
	int min, max;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);
	if (!g_at_result_iter_next(&iter, "+CIND:"))
		goto error;

	index = 1;

	while (g_at_result_iter_open_list(&iter)) {
		if (!g_at_result_iter_next_string(&iter, &str))
			goto error;

		if (!g_at_result_iter_open_list(&iter))
			goto error;

		while (g_at_result_iter_next_range(&iter, &min, &max))
			;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (!g_at_result_iter_close_list(&iter))
			goto error;

		if (g_str_equal("service", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_SERVICE] = index;
		else if (g_str_equal("call", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_CALL] = index;
		else if (g_str_equal("callsetup", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_CALLSETUP] = index;
		else if (g_str_equal("callheld", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_CALLHELD] = index;
		else if (g_str_equal("signal", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_SIGNAL] = index;
		else if (g_str_equal("roam", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_ROAM] = index;
		else if (g_str_equal("battchg", str) == TRUE)
			data->cind_pos[HFP_INDICATOR_BATTCHG] = index;

		index += 1;
	}

	g_at_chat_send(data->chat, "AT+CMER=3,0,0,1", cmer_prefix,
				cmer_cb, modem, NULL);
	return;

error:
	hfp_disable(modem);
}

static void brsf_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct hfp_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+BRSF:"))
		goto error;

	g_at_result_iter_next_number(&iter, (gint *)&data->ag_features);

	g_at_chat_send(data->chat, "AT+CIND=?", cind_prefix,
				cind_cb, modem, NULL);
	return;

error:
	hfp_disable(modem);
}

/* either oFono or Phone could request SLC connection */
static int service_level_connection(struct ofono_modem *modem,
				const char *tty)
{
	struct hfp_data *data = ofono_modem_get_data(modem);
	GIOChannel *io;
	GAtSyntax *syntax;
	GAtChat *chat;
	char buf[64];

	io = g_at_tty_open(tty, NULL);
	if (!io) {
		ofono_error("Service level connection failed: %s (%d)",
			strerror(errno), errno);
		return -EIO;
	}

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(io);

	if (!chat)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, hfp_debug, NULL);

	sprintf(buf, "AT+BRSF=%d", data->hf_features);

	g_at_chat_send(chat, buf, brsf_prefix,
				brsf_cb, modem, NULL);
	data->chat = chat;

	return -EINPROGRESS;
}

static int hfp_probe(struct ofono_modem *modem)
{
	struct hfp_data *data;

	data = g_try_new0(struct hfp_data, 1);
	if (!data)
		return -ENOMEM;

	data->hf_features |= HF_FEATURE_3WAY;
	data->hf_features |= HF_FEATURE_CLIP;
	data->hf_features |= HF_FEATURE_REMOTE_VOLUME_CONTROL;
	data->hf_features |= HF_FEATURE_ENHANCED_CALL_STATUS;
	data->hf_features |= HF_FEATURE_ENHANCED_CALL_CONTROL;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void hfp_remove(struct ofono_modem *modem)
{
	gpointer data = ofono_modem_get_data(modem);

	if (data)
		g_free(data);

	ofono_modem_set_data(modem, NULL);
}

/* power up hardware */
static int hfp_enable(struct ofono_modem *modem)
{
	const char *tty;
	int ret;

	DBG("%p", modem);

	tty = ofono_modem_get_string(modem, "Device");
	if (tty == NULL)
		return -EINVAL;

	ret = service_level_connection(modem, tty);

	return ret;
}

static int hfp_disable(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->chat)
		return 0;

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	memset(data->cind_val, 0, sizeof(data->cind_val));
	memset(data->cind_pos, 0, sizeof(data->cind_pos));

	ofono_modem_set_powered(modem, FALSE);

	return 0;
}

static void hfp_pre_sim(struct ofono_modem *modem)
{
	struct hfp_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);
	ofono_voicecall_create(modem, 0, "hfpmodem", data);
}

static void hfp_post_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static struct ofono_modem_driver hfp_driver = {
	.name		= "hfp",
	.probe		= hfp_probe,
	.remove		= hfp_remove,
	.enable		= hfp_enable,
	.disable	= hfp_disable,
	.pre_sim	= hfp_pre_sim,
	.post_sim	= hfp_post_sim,
};

static int hfp_init(void)
{
	DBG("");
	return ofono_modem_driver_register(&hfp_driver);
}

static void hfp_exit(void)
{
	ofono_modem_driver_unregister(&hfp_driver);
}

OFONO_PLUGIN_DEFINE(hfp, "Hands-Free Profile Plugins", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, hfp_init, hfp_exit)
