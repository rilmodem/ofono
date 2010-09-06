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
#include <ofono/sms.h>
#include <ofono/ssn.h>
#include <ofono/ussd.h>
#include <ofono/call-volume.h>
#include <ofono/voicecall.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/stk.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

#include <drivers/stemodem/caif_socket.h>
#include <drivers/stemodem/if_caif.h>

static const char *cpin_prefix[] = { "+CPIN:", NULL };

struct ste_data {
	GAtChat *chat;
	guint cpin_poll_source;
	guint cpin_poll_count;
	gboolean have_sim;
};

static int ste_probe(struct ofono_modem *modem)
{
	struct ste_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct ste_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void ste_remove(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	g_at_chat_unref(data->chat);

	if (data->cpin_poll_source > 0)
		g_source_remove(data->cpin_poll_source);

	g_free(data);
}

static void ste_debug(const char *str, void *user_data)
{
	ofono_info("%s", str);
}

static gboolean init_simpin_check(gpointer user_data);

static void simpin_check(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ste_data *data = ofono_modem_get_data(modem);

	/* Modem returns +CME ERROR: 10 if SIM is not ready. */
	if (!ok && result->final_or_pdu &&
			!strcmp(result->final_or_pdu, "+CME ERROR: 10") &&
			data->cpin_poll_count++ < 5) {
		data->cpin_poll_source =
			g_timeout_add_seconds(1, init_simpin_check, modem);
		return;
	}

	data->cpin_poll_count = 0;

	/* Modem returns ERROR if there is no SIM in slot. */
	data->have_sim = ok;

	ofono_modem_set_powered(modem, TRUE);
}

static gboolean init_simpin_check(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ste_data *data = ofono_modem_get_data(modem);

	data->cpin_poll_source = 0;

	g_at_chat_send(data->chat, "AT+CPIN?", cpin_prefix,
			simpin_check, modem, NULL);

	return FALSE;
}

static void cfun_enable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (!ok) {
		ofono_modem_set_powered(modem, FALSE);
		return;
	}

	init_simpin_check(modem);
}

static int ste_enable(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	GIOChannel *channel;
	GAtSyntax *syntax;
	const char *device;
	int fd;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device) {
		struct sockaddr_caif addr;
		int err;
		const char *interface;

		/* Create a CAIF socket for AT Service */
		fd = socket(AF_CAIF, SOCK_STREAM, CAIFPROTO_AT);
		if (fd < 0) {
			ofono_error("Failed to create CAIF socket for AT");
			return -EIO;
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
				return err;
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
			return err;
		}
	} else {
		fd = open(device, O_RDWR);
		if (fd < 0) {
			ofono_error("Failed to open device %s", device);
			return -EIO;
		}
	}

	channel = g_io_channel_unix_new(fd);
	if (!channel)  {
		close(fd);
		return -EIO;
	}
	g_io_channel_set_close_on_unref(channel, TRUE);

	syntax = g_at_syntax_new_gsm_permissive();

	data->chat = g_at_chat_new_blocking(channel, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(channel);

	if (!data->chat)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, ste_debug, NULL);

	g_at_chat_send(data->chat, "AT&F E0 V1 X4 &C1 +CMEE=1",
			NULL, NULL, NULL, NULL);
	g_at_chat_send(data->chat, "AT+CFUN=4", NULL, cfun_enable, modem, NULL);

	return -EINPROGRESS;
}

static void cfun_disable(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ste_data *data = ofono_modem_get_data(modem);

	DBG("");

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	if (ok)
		ofono_modem_set_powered(modem, FALSE);
}

static int ste_disable(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (!data->chat)
		return 0;

	g_at_chat_cancel_all(data->chat);
	g_at_chat_unregister_all(data->chat);
	g_at_chat_send(data->chat, "AT+CFUN=4", NULL,
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
	GAtChat *chat = data->chat;
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char const *command = online ? "AT+CFUN=1" : "AT+CFUN=4";

	DBG("modem %p %s", modem, online ? "online" : "offline");

	if (!cbd)
		goto error;

	if (g_at_chat_send(chat, command, NULL, set_online_cb, cbd, g_free))
		return;

error:
	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void ste_pre_sim(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	struct ofono_sim *sim;

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "atmodem", data->chat);
	sim = ofono_sim_create(modem, OFONO_VENDOR_MBM, "atmodem", data->chat);
	ofono_voicecall_create(modem, 0, "stemodem", data->chat);

	if (sim)
		ofono_sim_inserted_notify(sim, TRUE);
}

static void ste_post_sim(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_stk_create(modem, 0, "mbmmodem", data->chat);
	ofono_phonebook_create(modem, 0, "atmodem", data->chat);
}

static void ste_post_online(struct ofono_modem *modem)
{
	struct ste_data *data = ofono_modem_get_data(modem);
	struct ofono_message_waiting *mw;
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	ofono_radio_settings_create(modem, 0, "stemodem", data->chat);
	ofono_ussd_create(modem, 0, "atmodem", data->chat);
	ofono_call_forwarding_create(modem, 0, "atmodem", data->chat);
	ofono_call_settings_create(modem, 0, "atmodem", data->chat);
	ofono_netreg_create(modem, OFONO_VENDOR_MBM, "atmodem", data->chat);
	ofono_call_meter_create(modem, 0, "atmodem", data->chat);
	ofono_call_barring_create(modem, 0, "atmodem", data->chat);
	ofono_ssn_create(modem, 0, "atmodem", data->chat);
	ofono_sms_create(modem, 0, "atmodem", data->chat);
	ofono_call_volume_create(modem, 0, "atmodem", data->chat);

	gprs = ofono_gprs_create(modem, OFONO_VENDOR_MBM,
					"atmodem", data->chat);
	gc = ofono_gprs_context_create(modem, 0, "stemodem", data->chat);

	if (gprs && gc)
		ofono_gprs_add_context(gprs, gc);

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
