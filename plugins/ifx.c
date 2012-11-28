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

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <glib.h>
#include <gatchat.h>
#include <gatmux.h>
#include <gattty.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/voicecall.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/call-volume.h>
#include <ofono/message-waiting.h>
#include <ofono/sim.h>
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/audio-settings.h>
#include <ofono/gnss.h>
#include <ofono/stk.h>
#include <ofono/ctm.h>
#include <ofono/log.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

#define NUM_DLC  6

#define VOICE_DLC   0
#define NETREG_DLC  1
#define GPRS1_DLC   2
#define GPRS2_DLC   3
#define GPRS3_DLC   4
#define AUX_DLC     5

static char *dlc_prefixes[NUM_DLC] = { "Voice: ", "Net: ", "GPRS1: ",
					"GPRS2: ", "GPRS3: ", "Aux: " };

static const char *dlc_nodes[NUM_DLC] = { "/dev/ttyGSM1", "/dev/ttyGSM2",
					"/dev/ttyGSM3", "/dev/ttyGSM4",
					"/dev/ttyGSM5", "/dev/ttyGSM6" };

static const char *none_prefix[] = { NULL };
static const char *xgendata_prefix[] = { "+XGENDATA:", NULL };
static const char *xsimstate_prefix[] = { "+XSIMSTATE:", NULL };

struct ifx_data {
	GIOChannel *device;
	GAtMux *mux;
	GAtChat *dlcs[NUM_DLC];
	guint dlc_poll_count;
	guint dlc_poll_source;
	guint dlc_init_source;
	guint mux_init_timeout;
	guint frame_size;
	int mux_ldisc;
	int saved_ldisc;
	struct ofono_sim *sim;
};

static void ifx_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int ifx_probe(struct ofono_modem *modem)
{
	struct ifx_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct ifx_data, 1);
	if (data == NULL)
		return -ENOMEM;

	data->mux_ldisc = -1;
	data->saved_ldisc = -1;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void ifx_remove(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_free(data);
}

static void ifx_set_sim_state(struct ifx_data *data, int state)
{
	DBG("state %d", state);

	switch (state) {
	case 0:	/* SIM not present */
	case 6:	/* SIM Error */
	case 8:	/* SIM Technical Problem */
	case 9:	/* SIM Removed */
		ofono_sim_inserted_notify(data->sim, FALSE);
		break;
	case 1:	/* PIN verification needed */
	case 4:	/* PUK verification needed */
	case 5:	/* SIM permanently blocked */
	case 7:	/* ready for attach (+COPS) */
		ofono_sim_inserted_notify(data->sim, TRUE);
		break;
	case 2:	/* PIN verification not needed – Ready */
	case 3:	/* PIN verified – Ready */
		/*
		 * State 3 is handled in the SIM atom driver
		 * while for state 2 we should be waiting for state 7
		 */
		break;
	case 10: /* SIM Reactivating */
	case 11: /* SIM Reactivated */
	case 12: /* SIM SMS Caching Completed */
	case 99: /* SIM State Unknown */
		ofono_warn("Unhandled SIM state %d received", state);
		break;
	default:
		ofono_warn("Unknown SIM state %d received", state);
		break;
	}
}

static void xsim_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);

	GAtResultIter iter;
	int state;

	if (data->sim == NULL)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XSIM:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	ifx_set_sim_state(data, state);
}

static void xsimstate_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	int mode;
	int state;

	DBG("");

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XSIMSTATE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	if (!g_at_result_iter_next_number(&iter, &state))
		return;

	ifx_set_sim_state(data, state);
}

static void shutdown_device(struct ifx_data *data)
{
	int i, fd;

	DBG("");

	if (data->dlc_init_source > 0) {
		g_source_remove(data->dlc_init_source);
		data->dlc_init_source = 0;
	}

	for (i = 0; i < NUM_DLC; i++) {
		if (data->dlcs[i] == NULL)
			continue;

		g_at_chat_unref(data->dlcs[i]);
		data->dlcs[i] = NULL;
	}

	if (data->mux) {
		g_at_mux_shutdown(data->mux);
		g_at_mux_unref(data->mux);
		data->mux = NULL;
		goto done;
	}

	fd = g_io_channel_unix_get_fd(data->device);

	if (ioctl(fd, TIOCSETD, &data->saved_ldisc) < 0)
		ofono_warn("Failed to restore line discipline");

done:
	g_io_channel_unref(data->device);
	data->device = NULL;
}

static void dlc_disconnect(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);

	DBG("");

	ofono_warn("Disconnect of modem channel");

	shutdown_device(data);
}

static GAtChat *create_chat(GIOChannel *channel, struct ofono_modem *modem,
								char *debug)
{
	GAtSyntax *syntax;
	GAtChat *chat;

	if (channel == NULL)
		return NULL;

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_chat_new(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (chat == NULL)
		return NULL;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, ifx_debug, debug);

	g_at_chat_set_disconnect_function(chat, dlc_disconnect, modem);

	return chat;
}

static void xgendata_query(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);
	GAtResultIter iter;
	const char *gendata;

	DBG("");

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XGENDATA:"))
		goto error;

	if (!g_at_result_iter_next_string(&iter, &gendata))
		goto error;

	DBG("\n%s", gendata);

	/* switch to GSM character set instead of IRA */
	g_at_chat_send(data->dlcs[AUX_DLC], "AT+CSCS=\"GSM\"", none_prefix,
							NULL, NULL, NULL);

	/* disable UART for power saving */
	g_at_chat_send(data->dlcs[AUX_DLC], "AT+XPOW=0,0,0", none_prefix,
							NULL, NULL, NULL);

	/* notify that the modem is ready so that pre_sim gets called */
	ofono_modem_set_powered(modem, TRUE);

	g_at_chat_register(data->dlcs[AUX_DLC], "+XSIM:", xsim_notify,
						FALSE, modem, NULL);

	/* enable XSIM and XLOCK notifications */
	g_at_chat_send(data->dlcs[AUX_DLC], "AT+XSIMSTATE=1", none_prefix,
						NULL, NULL, NULL);

	g_at_chat_send(data->dlcs[AUX_DLC], "AT+XSIMSTATE?", xsimstate_prefix,
					xsimstate_query, modem, NULL);

	return;

error:
	shutdown_device(data);
	ofono_modem_set_powered(modem, FALSE);
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!ok) {
		shutdown_device(data);
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	g_at_chat_send(data->dlcs[AUX_DLC], "AT+XGENDATA", xgendata_prefix,
					xgendata_query, modem, NULL);
}

static gboolean dlc_setup(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("");

	for (i = 0; i < NUM_DLC; i++)
		g_at_chat_send(data->dlcs[i], "ATE0 +CMEE=1", NULL,
						NULL, NULL, NULL);

	g_at_chat_set_slave(data->dlcs[GPRS1_DLC], data->dlcs[NETREG_DLC]);
	g_at_chat_set_slave(data->dlcs[GPRS2_DLC], data->dlcs[NETREG_DLC]);
	g_at_chat_set_slave(data->dlcs[GPRS3_DLC], data->dlcs[NETREG_DLC]);

	g_at_chat_send(data->dlcs[AUX_DLC], "AT+CFUN=4", NULL,
					cfun_enable, modem, NULL);

	data->dlc_init_source = 0;

	return FALSE;
}

static gboolean dlc_ready_check(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);
	struct stat st;
	int i;

	DBG("");

	data->dlc_poll_count++;

	if (stat(dlc_nodes[AUX_DLC], &st) < 0) {
		/* only possible error is ENOENT */
		if (data->dlc_poll_count > 6)
			goto error;

		return TRUE;
	}

	for (i = 0; i < NUM_DLC; i++) {
		GIOChannel *channel = g_at_tty_open(dlc_nodes[i], NULL);

		data->dlcs[i] = create_chat(channel, modem, dlc_prefixes[i]);
		if (data->dlcs[i] == NULL) {
			ofono_error("Failed to open %s", dlc_nodes[i]);
			goto error;
		}
	}

	data->dlc_poll_source = 0;

	/* iterate through mainloop */
	data->dlc_init_source = g_timeout_add_seconds(0, dlc_setup, modem);

	return FALSE;

error:
	data->dlc_poll_source = 0;

	shutdown_device(data);
	ofono_modem_set_powered(modem, FALSE);

	return FALSE;
}

static void setup_internal_mux(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("");

	data->mux = g_at_mux_new_gsm0710_basic(data->device, data->frame_size);
	if (data->mux == NULL)
		goto error;

	if (getenv("OFONO_MUX_DEBUG"))
		g_at_mux_set_debug(data->mux, ifx_debug, "MUX: ");

	g_at_mux_start(data->mux);

	for (i = 0; i < NUM_DLC; i++) {
		GIOChannel *channel = g_at_mux_create_channel(data->mux);

		data->dlcs[i] = create_chat(channel, modem, dlc_prefixes[i]);
		if (data->dlcs[i] == NULL) {
			ofono_error("Failed to create channel");
			goto error;
		}
	}

	/* wait for DLC creation to settle */
	data->dlc_init_source = g_timeout_add(500, dlc_setup, modem);

	return;

error:
	shutdown_device(data);
	ofono_modem_set_powered(modem, FALSE);
}

static void mux_setup_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);
	int fd;

	DBG("");

	if (data->mux_init_timeout > 0) {
		g_source_remove(data->mux_init_timeout);
		data->mux_init_timeout = 0;
	}

	g_at_chat_unref(data->dlcs[AUX_DLC]);
	data->dlcs[AUX_DLC] = NULL;

	if (!ok)
		goto error;

	if (data->mux_ldisc < 0) {
		ofono_info("Using internal multiplexer");
		setup_internal_mux(modem);
		return;
	}

	fd = g_io_channel_unix_get_fd(data->device);

	if (ioctl(fd, TIOCGETD, &data->saved_ldisc) < 0) {
		ofono_error("Failed to get current line discipline");
		goto error;
	}

	if (ioctl(fd, TIOCSETD, &data->mux_ldisc) < 0) {
		ofono_error("Failed to set multiplexer line discipline");
		goto error;
	}

	data->dlc_poll_count = 0;
	data->dlc_poll_source = g_timeout_add_seconds(1, dlc_ready_check,
								modem);

	return;

error:
	data->saved_ldisc = -1;

	g_io_channel_unref(data->device);
	data->device = NULL;

	ofono_modem_set_powered(modem, FALSE);
}

static gboolean mux_timeout_cb(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);

	ofono_error("Timeout with multiplexer setup");

	data->mux_init_timeout = 0;

	g_at_chat_unref(data->dlcs[AUX_DLC]);
	data->dlcs[AUX_DLC] = NULL;

	g_io_channel_unref(data->device);
	data->device = NULL;

	ofono_modem_set_powered(modem, FALSE);

	return FALSE;
}

static int ifx_enable(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	const char *device, *ldisc;
	GAtSyntax *syntax;
	GAtChat *chat;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL)
		return -EINVAL;

	DBG("%s", device);

	ldisc = ofono_modem_get_string(modem, "LineDiscipline");
	if (ldisc != NULL) {
		data->mux_ldisc = atoi(ldisc);
		ofono_info("Using multiplexer line discipline %d",
							data->mux_ldisc);
	}

	data->device = g_at_tty_open(device, NULL);
	if (data->device == NULL)
		return -EIO;

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_chat_new(data->device, syntax);
	g_at_syntax_unref(syntax);

	if (chat == NULL) {
		g_io_channel_unref(data->device);
		return -EIO;
	}

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(chat, ifx_debug, "Master: ");

	g_at_chat_send(chat, "ATE0 +CMEE=1", NULL,
					NULL, NULL, NULL);

	/* Enable multiplexer */
	data->frame_size = 1509;

	g_at_chat_send(chat, "AT+CMUX=0,0,,1509,10,3,30,,", NULL,
					mux_setup_cb, modem, NULL);

	data->mux_init_timeout = g_timeout_add_seconds(5, mux_timeout_cb,
								modem);

	data->dlcs[AUX_DLC] = chat;

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ifx_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (data->dlc_poll_source > 0) {
		g_source_remove(data->dlc_poll_source);
		data->dlc_poll_source = 0;
	}

	shutdown_device(data);

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int ifx_disable(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("%p", modem);

	for (i = 0; i < NUM_DLC; i++) {
		g_at_chat_cancel_all(data->dlcs[i]);
		g_at_chat_unregister_all(data->dlcs[i]);
	}

	g_at_chat_send(data->dlcs[AUX_DLC], "AT+CFUN=0", NULL,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));
	cb(&error, cbd->data);
}

static void ifx_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("%p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(data->dlcs[AUX_DLC], command, none_prefix,
					set_online_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void ifx_pre_sim(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	data->sim = ofono_sim_create(modem, OFONO_VENDOR_IFX,
					"atmodem", data->dlcs[AUX_DLC]);
	ofono_voicecall_create(modem, 0, "ifxmodem", data->dlcs[VOICE_DLC]);
	ofono_audio_settings_create(modem, 0,
					"ifxmodem", data->dlcs[VOICE_DLC]);
	ofono_ctm_create(modem, 0, "ifxmodem", data->dlcs[AUX_DLC]);
}

static void ifx_post_sim(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_stk_create(modem, 0, "ifxmodem", data->dlcs[AUX_DLC]);
	ofono_phonebook_create(modem, OFONO_VENDOR_IFX,
					"atmodem", data->dlcs[AUX_DLC]);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_radio_settings_create(modem, 0, "ifxmodem", data->dlcs[AUX_DLC]);

	ofono_sms_create(modem, OFONO_VENDOR_IFX,
					"atmodem", data->dlcs[AUX_DLC]);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_IFX,
					"atmodem", data->dlcs[NETREG_DLC]);
	if (gprs == NULL)
		return;

	if (data->mux_ldisc < 0) {
		gc = ofono_gprs_context_create(modem, 0,
					"ifxmodem", data->dlcs[GPRS1_DLC]);
		if (gc)
			ofono_gprs_add_context(gprs, gc);

		gc = ofono_gprs_context_create(modem, 0,
					"ifxmodem", data->dlcs[GPRS2_DLC]);
		if (gc)
			ofono_gprs_add_context(gprs, gc);

		gc = ofono_gprs_context_create(modem, 0,
					"ifxmodem", data->dlcs[GPRS3_DLC]);
		if (gc)
			ofono_gprs_add_context(gprs, gc);
	}
}

static void ifx_post_online(struct ofono_modem *modem)
{
	struct ifx_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;

	DBG("%p", modem);

	ofono_netreg_create(modem, OFONO_VENDOR_IFX,
					"atmodem", data->dlcs[NETREG_DLC]);

	ofono_cbs_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_ussd_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);

	ofono_gnss_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);

	ofono_call_settings_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_call_meter_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_call_barring_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);
	ofono_call_volume_create(modem, 0, "atmodem", data->dlcs[AUX_DLC]);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver ifx_driver = {
	.name		= "ifx",
	.probe		= ifx_probe,
	.remove		= ifx_remove,
	.enable		= ifx_enable,
	.disable	= ifx_disable,
	.set_online	= ifx_set_online,
	.pre_sim	= ifx_pre_sim,
	.post_sim	= ifx_post_sim,
	.post_online	= ifx_post_online,
};

static int ifx_init(void)
{
	return ofono_modem_driver_register(&ifx_driver);
}

static void ifx_exit(void)
{
	ofono_modem_driver_unregister(&ifx_driver);
}

OFONO_PLUGIN_DEFINE(ifx, "Infineon modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ifx_init, ifx_exit)
