/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "gatmux.h"

static int do_connect(const char *address, unsigned short port)
{
	struct sockaddr_in addr;
	int sk, err;

	sk = socket(PF_INET, SOCK_STREAM, 0);
	if (sk < 0)
		return sk;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(address);
	addr.sin_port = htons(port);

	err = connect(sk, (struct sockaddr *) &addr, sizeof(addr));
	if (err < 0) {
		close(sk);
		return err;
	}

	return sk;
}

static GMainLoop *mainloop;

static gboolean cleanup_callback(gpointer data)
{
	GAtChat *chat = data;

	g_at_chat_shutdown(chat);

	g_at_chat_unref(chat);

	g_main_loop_quit(mainloop);

	return FALSE;
}

static void chat_callback(gboolean ok, GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	g_print("chat: callback [ok %d]\n", ok);

	g_print("%s\n", g_at_result_final_response(result));

	g_idle_add(cleanup_callback, user_data);
}

static void mux_debug(const char *str, void *data)
{
	g_print("%s: %s\n", (char *) data, str);
}

static gboolean idle_callback(gpointer data)
{
	GAtMux *mux = data;
	GAtChat *chat;
	GAtSyntax *syntax;

	g_print("idle: callback\n");

	syntax = g_at_syntax_new_gsmv1();
	chat = g_at_mux_create_chat(mux, syntax);
	g_at_syntax_unref(syntax);

	if (!chat) {
		g_printerr("chat failed\n");
		g_main_loop_quit(mainloop);
		return FALSE;
	}

	g_at_chat_set_debug(chat, mux_debug, "CHAT");

	g_at_chat_send(chat, "AT+CGMI", NULL, NULL, NULL, NULL);

	g_at_chat_send(chat, "AT+CGMR", NULL, chat_callback, chat, NULL);

	return FALSE;
}

static void test_mux(void)
{
	GIOChannel *io;
	GAtMux *mux;
	int sk;

	sk= do_connect("192.168.0.202", 2000);
	if (sk < 0) {
		g_printerr("connect failed\n");
		return;
	}

	io = g_io_channel_unix_new(sk);
	mux = g_at_mux_new(io);
	g_io_channel_unref(io);

	if (!mux) {
		g_printerr("mux failed\n");
		close(sk);
		return;
	}

	g_at_mux_set_debug(mux, mux_debug, "MUX");

	g_io_channel_set_close_on_unref(io, TRUE);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_idle_add(idle_callback, mux);

	g_main_loop_run(mainloop);

	g_at_mux_unref(mux);

	g_main_loop_unref(mainloop);
}

static void test_basic(void)
{
	if (g_test_trap_fork(60 * 1000 * 1000, 0) == TRUE) {
		test_mux();
		exit(0);
	}

	g_test_trap_assert_passed();
	//g_test_trap_assert_stderr("failed");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testmux/basic", test_basic);

	return g_test_run();
}
