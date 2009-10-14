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
static GAtMux *mux;

static gboolean cleanup_callback(gpointer data)
{
	g_at_mux_unref(mux);

	g_main_loop_quit(mainloop);

	return FALSE;
}

static gboolean chat_cleanup(gpointer data)
{
	GAtChat *chat = data;

	g_at_chat_shutdown(chat);
	g_at_chat_unref(chat);

	return FALSE;
}

static void chat_callback(gboolean ok, GAtResult *result, gpointer user_data)
{
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	g_print("chat: callback [ok %d]\n", ok);

	g_print("%s\n", g_at_result_final_response(result));

	g_idle_add(chat_cleanup, user_data);
}

static void mux_debug(const char *str, void *data)
{
	g_print("%s: %s\n", (char *) data, str);
}

static void mux_setup(GAtMux *m, gpointer data)
{
	GAtChat *chat = data;
	GIOChannel *io;
	GAtSyntax *syntax;

	mux = m;

	g_print("mux_setup: %p\n", mux);

	if (mux == NULL) {
		g_at_chat_unref(chat);
		g_main_loop_quit(mainloop);
		return;
	}

	g_at_mux_start(mux);

	io = g_at_mux_create_channel(mux);
	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(io);

	g_at_chat_set_debug(chat, mux_debug, "CHAT1");
	g_at_chat_set_wakeup_command(chat, "\r", 1000, 5000);
	g_at_chat_send(chat, "AT+CGMI", NULL, NULL, NULL, NULL);
	g_at_chat_send(chat, "AT+CGMR", NULL, chat_callback, chat, NULL);

	io = g_at_mux_create_channel(mux);
	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(io);

	g_at_chat_set_debug(chat, mux_debug, "CHAT2");
	g_at_chat_set_wakeup_command(chat, "\r", 1000, 5000);
	g_at_chat_send(chat, "AT+CGMI", NULL, NULL, NULL, NULL);
	g_at_chat_send(chat, "AT+CGMR", NULL, chat_callback, chat, NULL);

	io = g_at_mux_create_channel(mux);
	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(io);

	g_at_chat_set_debug(chat, mux_debug, "CHAT3");
	g_at_chat_set_wakeup_command(chat, "\r", 1000, 5000);
	g_at_chat_send(chat, "AT+CGMI", NULL, NULL, NULL, NULL);
	g_at_chat_send(chat, "AT+CGMR", NULL, chat_callback, chat, NULL);

	io = g_at_mux_create_channel(mux);
	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);
	g_io_channel_unref(io);

	g_at_chat_set_debug(chat, mux_debug, "CHAT4");
	g_at_chat_set_wakeup_command(chat, "\r", 1000, 5000);
	g_at_chat_send(chat, "AT+CGMI", NULL, NULL, NULL, NULL);
	g_at_chat_send(chat, "AT+CGMR", NULL, chat_callback, chat, NULL);

	g_timeout_add_seconds(7, cleanup_callback, NULL);
}

static void mux_init(gboolean ok, GAtResult *result, gpointer data)
{
	GAtChat *chat = data;

	g_print("mux_init: %d\n", ok);

	if (ok == FALSE) {
		g_at_chat_unref(chat);
		g_main_loop_quit(mainloop);
		return;
	}

	g_at_mux_setup_gsm0710(chat, mux_setup, chat, NULL);
}

static void test_mux(void)
{
	GIOChannel *io;
	GAtChat *chat;
	GAtSyntax *syntax;
	int sk;

	sk= do_connect("192.168.0.202", 2000);
	if (sk < 0) {
		g_printerr("connect failed\n");
		return;
	}

	mux = NULL;
	io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(io, TRUE);

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_unref(io);

	if (!chat) {
		g_printerr("Chat creation failed\n");
		return;
	}

	g_at_chat_set_debug(chat, mux_debug, "MUX");
	g_at_chat_set_wakeup_command(chat, "\r", 1000, 5000);
	g_at_chat_send(chat, "ATE0", NULL, mux_init, chat, NULL);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
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
