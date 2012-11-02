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

#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>

#include <glib.h>
#include <gatmux.h>
#include <gatchat.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/stk.h>

#include <drivers/atmodem/atutil.h>
#include <drivers/atmodem/vendor.h>

#include "ofono.h"

static struct ofono_modem *stktest;

static const char *none_prefix[] = { NULL };

struct stktest_data {
	GAtChat *chat;
};

static int stktest_probe(struct ofono_modem *modem)
{
	struct stktest_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct stktest_data, 1);
	if (data == NULL)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void stktest_remove(struct ofono_modem *modem)
{
	struct stktest_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_free(data);
	ofono_modem_set_data(modem, NULL);
}

static void stktest_debug(const char *str, void *prefix)
{
	ofono_info("%s%s", (const char *) prefix, str);
}

static void stktest_disconnected(gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct stktest_data *data = ofono_modem_get_data(modem);

	DBG("");

	ofono_modem_set_powered(modem, FALSE);

	g_at_chat_unref(data->chat);
	data->chat = NULL;
}

static int connect_socket(const char *address, int port)
{
	struct sockaddr_in addr;
	int sk;
	int err;

	sk = socket(PF_INET, SOCK_STREAM, 0);
	if (sk < 0)
		return -EINVAL;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(address);
	addr.sin_port = htons(port);

	err = connect(sk, (struct sockaddr *) &addr, sizeof(addr));
	if (err < 0) {
		close(sk);
		return -errno;
	}

	return sk;
}

static int stktest_enable(struct ofono_modem *modem)
{
	struct stktest_data *data = ofono_modem_get_data(modem);
	GIOChannel *io;
	GAtSyntax *syntax;
	int sk;

	DBG("%p", modem);

	sk = connect_socket("127.0.0.1", 12765);
	if (sk < 0)
		return sk;

	io = g_io_channel_unix_new(sk);
	if (io == NULL) {
		close(sk);
		return -ENOMEM;
	}

	syntax = g_at_syntax_new_gsmv1();
	data->chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(io);

	if (data->chat == NULL)
		return -ENOMEM;

	if (getenv("OFONO_AT_DEBUG"))
		g_at_chat_set_debug(data->chat, stktest_debug, "");

	g_at_chat_set_disconnect_function(data->chat,
						stktest_disconnected, modem);

	return 0;
}

static void set_online_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t callback = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	callback(&error, cbd->data);
}

static void stktest_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct stktest_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char buf[64];

	DBG("%p", modem);

	snprintf(buf, sizeof(buf), "AT+CFUN=%d", online ? 1 : 4);

	if (g_at_chat_send(data->chat, buf, none_prefix,
				set_online_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, user_data);
}

static int stktest_disable(struct ofono_modem *modem)
{
	struct stktest_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	g_at_chat_unref(data->chat);
	data->chat = NULL;

	return 0;
}

static void stktest_pre_sim(struct ofono_modem *modem)
{
	DBG("%p", modem);
}

static void stktest_post_sim(struct ofono_modem *modem)
{
	struct stktest_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_stk_create(modem, OFONO_VENDOR_PHONESIM, "atmodem", data->chat);
}

static void stktest_post_online(struct ofono_modem *modem)
{
}

static struct ofono_modem_driver stktest_driver = {
	.modem_type	= OFONO_MODEM_TYPE_TEST,
	.name		= "stktest",
	.probe		= stktest_probe,
	.remove		= stktest_remove,
	.enable		= stktest_enable,
	.disable	= stktest_disable,
	.set_online	= stktest_set_online,
	.pre_sim	= stktest_pre_sim,
	.post_sim	= stktest_post_sim,
	.post_online	= stktest_post_online,
};

static int stktest_init(void)
{
	int err;

	err = ofono_modem_driver_register(&stktest_driver);
	if (err < 0)
		return err;

	stktest = ofono_modem_create("stktest", "stktest");
	ofono_modem_register(stktest);

	return 0;
}

static void stktest_exit(void)
{
	ofono_modem_remove(stktest);
	ofono_modem_driver_unregister(&stktest_driver);
}

OFONO_PLUGIN_DEFINE(stktest, "STK End-to-End tester driver", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, stktest_init, stktest_exit)
