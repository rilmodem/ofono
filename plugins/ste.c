/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010 ST-Ericsson AB.
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

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>

#include <glib.h>
#include <gatchat.h>

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
#include <ofono/cbs.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/call-volume.h>
#include <ofono/voicecall.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/stk.h>
#include <ofono/gnss.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

#include <drivers/stemodem/caif_socket.h>
#include <drivers/stemodem/if_caif.h>

#define NUM_CHAT	6
#define AT_DEFAULT	0
#define AT_NET		1
#define AT_VOICE	2
#define AT_GPRS	3
#define AT_SIM		4
#define AT_GNSS	5

#define MAX_PDP_CONTEXTS	4

static char *chat_prefixes[NUM_CHAT] = { "Default: ", "Net: ", "Voice: ",
					 "GPRS: ", "SIM: ", "GNSS:" };

struct ste_data {
	GAtChat *chat[NUM_CHAT];
	gboolean have_sim;
	struct ofono_sim *sim;
};

enum ste_sim_state {
	SIM_STATE_NULL = 0,
	SIM_STATE_AWAITING_APP,
	SIM_STATE_BLOCKED,
	SIM_STATE_BLOCKED_FOREVER,
	SIM_STATE_WAIT_FOR_PIN,
	SIM_STATE_ACTIVE,
	SIM_STATE_TERMINATING,
	SIM_STATE_POWER_OFF
};

static int ste_probe(struct ofono_modem *modem)
{
	struct ste_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct ste_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void ste_remove(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	for (i = 0; i < NUM_CHAT; i++)
		g_at_chat_unref(data->chat[i]);

	g_free(data);
}

static void ste_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static void handle_sim_status(int status, struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	DBG("SIM status:%d\n", status);

	switch (status) {
	case SIM_STATE_WAIT_FOR_PIN:
	case SIM_STATE_ACTIVE:
	case SIM_STATE_NULL:
	case SIM_STATE_AWAITING_APP:
	case SIM_STATE_BLOCKED:
	case SIM_STATE_BLOCKED_FOREVER:
	case SIM_STATE_TERMINATING:
		if (data->have_sim == FALSE) {
			if (data->sim)
				ofono_sim_inserted_notify(data->sim, TRUE);

			data->have_sim = TRUE;
		}
		break;
	case SIM_STATE_POWER_OFF:
		if (data->have_sim == TRUE) {
			if (data->sim)
				ofono_sim_inserted_notify(data->sim, FALSE);

			data->have_sim = FALSE;
		}
		break;
	}
}

static void handle_sim_state(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	int simnr, status;
	GAtResultIter iter;

	DBG("ok:%d", ok);

	if (!ok)
		return;

	g_at_result_iter_init(&iter, result);

	ofono_modem_set_powered(modem, TRUE);

	if (!g_at_result_iter_next(&iter, "*ESIMSR:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &simnr))
		return;

	if (!g_at_result_iter_next_number(&iter, &status))
		return;

	handle_sim_status(status, modem);
}

static gboolean init_sim_reporting(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ste_data *data = ofono_modem_get_data(modem);

	data->have_sim = FALSE;

	g_at_chat_send(data->chat[AT_SIM], "AT*ESIMSR=1;*ESIMSR?", NULL,
			handle_sim_state, modem, NULL);

	return FALSE;
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ste_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("");

	if (!ok) {
		ofono_modem_set_powered(modem, FALSE);

		for (i = 0; i < NUM_CHAT; i++) {
			g_at_chat_cancel_all(data->chat[i]);
			g_at_chat_unregister_all(data->chat[i]);
			g_at_chat_unref(data->chat[i]);
			data->chat[i] = NULL;
		}

		return;
	}

	init_sim_reporting(modem);
}

static GIOChannel *ste_create_channel(struct ofono_modem *modem)
{
	GIOChannel *channel;
	const char *device;
	int fd;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (device == NULL) {
		struct sockaddr_caif addr;
		int err;
		const char *interface;

		/* Create a CAIF socket for AT Service */
		fd = socket(AF_CAIF, SOCK_STREAM, CAIFPROTO_AT);
		if (fd < 0) {
			ofono_error("Failed to create CAIF socket for AT");
			return NULL;
		}

		/* Bind CAIF socket to specified interface */
		interface = ofono_modem_get_string(modem, "Interface");
		if (interface) {
			struct ifreq ifreq;

			memset(&ifreq, 0, sizeof(ifreq));
			strcpy(ifreq.ifr_name, interface);
			err = setsockopt(fd, SOL_SOCKET,
					SO_BINDTODEVICE, &ifreq, sizeof(ifreq));
			if (err < 0) {
				ofono_error("Failed to bind caif socket "
					"to interface");
				close(fd);
				return NULL;
			}
		}

		memset(&addr, 0, sizeof(addr));
		addr.family = AF_CAIF;
		addr.u.at.type = CAIF_ATTYPE_PLAIN;

		/* Connect to the AT Service at the modem */
		err = connect(fd, (struct sockaddr *) &addr, sizeof(addr));
		if (err < 0) {
			ofono_error("Failed to connect CAIF socket for AT");
			close(fd);
			return NULL;
		}
	} else {
		fd = open(device, O_RDWR);
		if (fd < 0) {
			ofono_error("Failed to open device %s", device);
			return NULL;
		}
	}

	channel = g_io_channel_unix_new(fd);
	if (channel == NULL)  {
		close(fd);
		return NULL;
	}

	g_io_channel_set_close_on_unref(channel, TRUE);

	return channel;
}

static void esimsr_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	int status;
	GAtResultIter iter;
	DBG("");

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "*ESIMSR:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &status))
		return;

	handle_sim_status(status, modem);
}

static int ste_enable(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	GIOChannel *channel;
	GAtSyntax *syntax;
	int i;

	for (i = 0; i < NUM_CHAT; i++) {
		channel = ste_create_channel(modem);
		syntax = g_at_syntax_new_gsm_permissive();
		data->chat[i] = g_at_chat_new_blocking(channel, syntax);

		if (data->chat[i] == NULL) {
			g_io_channel_unref(channel);
			g_at_syntax_unref(syntax);
			DBG("Failed to create AT chat %s", chat_prefixes[i]);
			goto error;
		}

		if (getenv("OFONO_AT_DEBUG"))
			g_at_chat_set_debug(data->chat[i], ste_debug,
						chat_prefixes[i]);

		g_at_chat_send(data->chat[i], "AT&F E0 V1 X4 &C1 +CMEE=1",
				NULL, NULL, NULL, NULL);

		/* All STE modems support UTF-8 */
		g_at_chat_send(data->chat[i], "AT+CSCS=\"UTF-8\"",
				NULL, NULL, NULL, NULL);

		g_io_channel_unref(channel);
		g_at_syntax_unref(syntax);
	}

	g_at_chat_send(data->chat[AT_DEFAULT], "AT+CFUN=4", NULL, cfun_enable,
				modem, NULL);

	g_at_chat_register(data->chat[AT_SIM], "*ESIMSR:", esimsr_notify,
				FALSE, modem, NULL);

	return -EINPROGRESS;

error:
	/* Unref open chats if any */
	while (i--) {
		g_at_chat_unref(data->chat[i]);
		data->chat[i] = NULL;
	}

	return -EIO;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ste_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("");

	for (i = 0; i < NUM_CHAT; i++) {
		g_at_chat_unref(data->chat[i]);
		data->chat[i] = NULL;
	}

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int ste_disable(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	int i;

	DBG("%p", modem);

	for (i = 0; i < NUM_CHAT; i++) {
		g_at_chat_cancel_all(data->chat[i]);
		g_at_chat_unregister_all(data->chat[i]);
	}
	g_at_chat_send(data->chat[AT_DEFAULT], "AT+CFUN=4", NULL,
					cfun_disable, modem, NULL);

	return -EINPROGRESS;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	if (ok)
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	else
		CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ste_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	GAtChat *chat = data->chat[AT_DEFAULT];
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (g_at_chat_send(chat, command, NULL, set_online_cb, cbd, g_free))
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ste_pre_sim(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat[AT_DEFAULT]);
	data->sim = ofono_sim_create(modem, OFONO_VENDOR_MBM, "atmodem",
					data->chat[AT_SIM]);
	ofono_voicecall_create(modem, 0, "stemodem", data->chat[AT_VOICE]);
}

static void ste_post_sim(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_stk_create(modem, 0, "mbmmodem", data->chat[AT_SIM]);
	ofono_phonebook_create(modem, 0, "atmodem", data->chat[AT_SIM]);
	ofono_radio_settings_create(modem, 0, "stemodem", data->chat[AT_NET]);

	ofono_sms_create(modem, 0, "atmodem", data->chat[AT_DEFAULT]);
}

static void ste_post_online(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;
	int i;

	DBG("%p", modem);

	ofono_ussd_create(modem, 0, "atmodem", data->chat[AT_DEFAULT]);
	ofono_call_forwarding_create(modem, 0,
					"atmodem", data->chat[AT_DEFAULT]);
	ofono_call_settings_create(modem, 0, "atmodem", data->chat[AT_DEFAULT]);
	ofono_netreg_create(modem, OFONO_VENDOR_MBM,
					"atmodem", data->chat[AT_NET]);
	ofono_call_meter_create(modem, 0, "atmodem", data->chat[AT_DEFAULT]);
	ofono_call_barring_create(modem, 0, "atmodem", data->chat[AT_DEFAULT]);
	ofono_call_volume_create(modem, 0, "atmodem", data->chat[AT_DEFAULT]);
	ofono_cbs_create(modem, 0, "atmodem", data->chat[AT_DEFAULT]);
	ofono_gnss_create(modem, OFONO_VENDOR_STE, "atmodem",
				data->chat[AT_GNSS]);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_MBM,
					"atmodem", data->chat[AT_GPRS]);

	if (gprs) {
		for (i = 0; i < MAX_PDP_CONTEXTS; i++) {
			gc = ofono_gprs_context_create(modem, 0, "stemodem",
					data->chat[AT_GPRS]);
			if (gc == NULL)
				break;

			ofono_gprs_add_context(gprs, gc);
		}
	}

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver ste_driver = {
	.name		= "ste",
	.probe		= ste_probe,
	.remove		= ste_remove,
	.enable		= ste_enable,
	.disable	= ste_disable,
	.set_online     = ste_set_online,
	.pre_sim	= ste_pre_sim,
	.post_sim	= ste_post_sim,
	.post_online    = ste_post_online,
};

static int ste_init(void)
{
	return ofono_modem_driver_register(&ste_driver);
}

static void ste_exit(void)
{
	ofono_modem_driver_unregister(&ste_driver);
}

OFONO_PLUGIN_DEFINE(ste, "ST-Ericsson modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, ste_init, ste_exit)
