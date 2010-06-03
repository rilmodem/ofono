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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

static const char *none_prefix[] = { NULL };
static const char *nwdmat_prefix[] = { "$NWDMAT:", NULL };

struct novatel_data {
	GAtChat *chat;
	GAtChat *gprs;
	gint dmat_mode;
};

static int novatel_probe(struct ofono_modem *modem)
{
	struct novatel_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct novatel_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void novatel_remove(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->chat);
	g_free(data);
}

static void novatel_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;
	ofono_info("%s%s", prefix, str);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (ok)
		ofono_modem_set_powered(modem, TRUE);
}

static void nwdmat_action(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (ok)
		data->dmat_mode = 1;

	g_at_chat_send(data->chat, "AT+CFUN=1", none_prefix,
					cfun_enable, modem, NULL);
}

static void nwdmat_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct novatel_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	gint dmat_mode;

	DBG("");

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "$NWDMAT:"))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &dmat_mode))
		goto error;

	if (dmat_mode == 1) {
		nwdmat_action(TRUE, result, user_data);
		return;
	}

	g_at_chat_send(data->chat, "AT$NWDMAT=1", nwdmat_prefix,
					nwdmat_action, modem, NULL);

	return;

error:
	nwdmat_action(FALSE, result, user_data);
}

static int novatel_enable(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);
	GAtSyntax *syntax;
	GIOChannel *channel;
	const char *device;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device)
		return -EINVAL;

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return -EIO;

	syntax = g_at_syntax_new_gsm_permissive();
	data->chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!data->chat)
		return -EIO;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, novatel_debug, "Modem:");

	g_at_chat_send(data->chat, "ATE0 +CMEE=1", none_prefix,
						NULL, NULL, NULL);

	/* Check mode of seconday port */
	g_at_chat_send(data->chat, "AT$NWDMAT?", nwdmat_prefix,
					nwdmat_query, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int novatel_disable(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->chat)
		return 0;

	if (data->gprs) {
		g_at_chat_cancel_all(data->gprs);
		g_at_chat_unregister_all(data->gprs);

		g_at_chat_unref(data->gprs);
		data->gprs = NULL;
	}

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);

	g_at_chat_send(data->chat, "AT$NWDMAT=0", nwdmat_prefix,
						NULL, NULL, NULL);

	g_at_chat_send(data->chat, "AT+CFUN=0", none_prefix,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void novatel_pre_sim(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	sim = ofono_sim_create(modem, 0, "atmodem", data->chat);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void creg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	int status, lac, ci, tech;

	DBG("");

	if (at_util_parse_reg_unsolicited(result, "+CREG:", &status,
						&lac, &ci, &tech, 0) == FALSE)
		return;

	ofono_netreg_status_notify(netreg, status, lac, ci, tech);
}

static void novatel_post_sim(struct ofono_modem *modem)
{
	struct novatel_data *data = ofono_modem_get_data(modem);
	struct ofono_netreg *netreg;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	GAtSyntax *syntax;
	GIOChannel *channel;
	const char *device;

	DBG("%p", modem);

	netreg = ofono_netreg_create(modem, OFONO_VENDOR_NOVATEL, "atmodem",
								data->chat);

	if (data->dmat_mode != 1)
		return;

	device = ofono_modem_get_string(modem, "Data");
	if (!device)
		return;

	channel = g_at_tty_open(device, NULL);
	if (!channel)
		return;

	syntax = g_at_syntax_new_gsm_permissive();
	data->gprs = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!data->gprs)
		return;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->gprs, novatel_debug, "GPRS:");

	g_at_chat_send(data->gprs, "ATE0 +CMEE=1", none_prefix,
						NULL, NULL, NULL);

	ofono_sms_create(modem, OFONO_VENDOR_NOVATEL, "atmodem", data->gprs);

	gprs = ofono_gprs_create(modem, 0, "atmodem", data->gprs);
	gc = ofono_gprs_context_create(modem, 0, "atmodem", data->gprs);

	if (gprs && gc) {
		ofono_gprs_add_context(gprs, gc);

		/* Handle CREG notifications from GPRS channel */
		g_at_chat_register(data->gprs, "+CREG:",
					creg_notify, FALSE, netreg, NULL);
	}
}

static struct ofono_modem_driver novatel_driver = {
	.name		= "novatel",
	.probe		= novatel_probe,
	.remove		= novatel_remove,
	.enable		= novatel_enable,
	.disable	= novatel_disable,
	.pre_sim	= novatel_pre_sim,
	.post_sim	= novatel_post_sim,
};

static int novatel_init(void)
{
	return ofono_modem_driver_register(&novatel_driver);
}

static void novatel_exit(void)
{
	ofono_modem_driver_unregister(&novatel_driver);
}

OFONO_PLUGIN_DEFINE(novatel, "Novatel Wireless modem driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, novatel_init, novatel_exit)
