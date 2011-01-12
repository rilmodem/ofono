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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "gatmux.h"
#include "gsm0710.h"

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

	sk = do_connect("192.168.0.202", 2000);
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

	if (chat == NULL) {
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

/* DLC 1, Open Channel */
static const guint8 basic_open[] = { 0xF9, 0x07, 0x3F, 0x01, 0xDE, 0xF9 };

/* DLC 1, Close Channel */
static char const basic_close[] = { 0xF9, 0x07, 0x53, 0x01, 0x3F, 0xF9 };

/* DLC 1, Data */
static const guint8 basic_data[] = { 0x12, 0x34, 0x56 };
static const guint8 basic_data_result[] =
	{ 0xF9, 0x07, 0xEF, 0x07, 0x12, 0x34, 0x56, 0xD3, 0xF9 };

/* DLC 1, Long Data */
static const guint8 basic_long_frame[] =
{	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
	0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0,
};

static const guint8 basic_long_frame_result[] =
{	0xF9, 0x07, 0xEF, 0x10, 0x01, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x12, 0x34, 0x56,
	0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x88, 0xF9
};

static void test_fill_basic(void)
{
	guint8 control_frame[6];
	guint8 data_frame[128];
	guint8 long_frame[256];
	int s;

	s = gsm0710_basic_fill_frame(control_frame, 1, GSM0710_OPEN_CHANNEL,
					NULL, 0);
	g_assert(s == sizeof(basic_open));
	g_assert(memcmp(basic_open, control_frame, s) == 0);

	s = gsm0710_basic_fill_frame(control_frame, 1, GSM0710_CLOSE_CHANNEL,
					NULL, 0);
	g_assert(s == sizeof(basic_close));
	g_assert(memcmp(basic_close, control_frame, s) == 0);

	s = gsm0710_basic_fill_frame(data_frame, 1, GSM0710_DATA,
					basic_data, sizeof(basic_data));
	g_assert(s == sizeof(basic_data_result));
	g_assert(memcmp(basic_data_result, data_frame, s) == 0);

	s = gsm0710_basic_fill_frame(long_frame, 1, GSM0710_DATA,
					basic_long_frame,
					sizeof(basic_long_frame));
	g_assert(s == sizeof(basic_long_frame_result));
	g_assert(memcmp(basic_long_frame_result, long_frame, s) == 0);
}

/* DLC 1, Open Channel */
static const guint8 advanced_open[] = { 0x7E, 0x07, 0x3F, 0x89, 0x7E };

/* DLC 1, Close Channel */
static const guint8 advanced_close[] = { 0x7E, 0x07, 0x53, 0xC8, 0x7E };

/* DLC 1, Data */
static const guint8 advanced_data[] = { 0x12, 0x34, 0x56 };
static const guint8 advanced_data_result[] =
	{ 0x7E, 0x07, 0xEF, 0x12, 0x34, 0x56, 0x05, 0x7E };

/* DLC 1, Quoted data */
static const guint8 advanced_quoted_data[] =
	{ 0x12, 0x34, 0x56, 0x7E, 0x78, 0x7D };
static const guint8 advanced_quoted_data_result[] =
	{ 0x7E, 0x07, 0xEF, 0x12, 0x34, 0x56, 0x7D, 0x5E, 0x78,
		0x7D, 0x5D, 0x05, 0x7E };

static void test_fill_advanced(void)
{
	guint8 control_frame[8];
	guint8 data_frame[128];
	int s;

	s = gsm0710_advanced_fill_frame(control_frame, 1, GSM0710_OPEN_CHANNEL,
					NULL, 0);
	g_assert(s == sizeof(advanced_open));
	g_assert(memcmp(advanced_open, control_frame, s) == 0);

	s = gsm0710_advanced_fill_frame(control_frame, 1, GSM0710_CLOSE_CHANNEL,
					NULL, 0);
	g_assert(s == sizeof(advanced_close));
	g_assert(memcmp(advanced_close, control_frame, s) == 0);

	s = gsm0710_advanced_fill_frame(data_frame, 1, GSM0710_DATA,
					advanced_data, sizeof(advanced_data));
	g_assert(s == sizeof(advanced_data_result));
	g_assert(memcmp(advanced_data_result, data_frame, s) == 0);

	s = gsm0710_advanced_fill_frame(data_frame, 1, GSM0710_DATA,
					advanced_quoted_data,
					sizeof(advanced_quoted_data));
	g_assert(s == sizeof(advanced_quoted_data_result));
	g_assert(memcmp(advanced_quoted_data_result, data_frame, s) == 0);
}

static guint8 basic_input[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xF9, 0x07, 0xEF,
	0x07, 0x12, 0x34, 0x56, 0xD3, 0xF9, 0x07, 0xEF, 0x07, 0x12, 0x34, 0x56,
	0xD3, 0xF9 };

static guint8 basic_input2[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xF9, 0x07, 0xEF,
	0x07, 0x12, 0x34, 0x56, 0xD3, 0xF9, 0xF9, 0x07, 0xEF, 0x07, 0x12,
	0x34, 0x56, 0xD3, 0xF9 };

static int basic_garbage_size = 4;
static int basic_frame_size = 7;

static const guint8 basic_output[] = { 0x12, 0x34, 0x56 };

static void test_extract_basic(void)
{
	int total = 0;
	int nread;
	guint8 dlc;
	guint8 ctrl;
	guint8 *frame;
	int frame_size;

	frame = NULL;
	frame_size = 0;

	nread = gsm0710_basic_extract_frame(basic_input + total,
						basic_garbage_size, &dlc, &ctrl,
						&frame, &frame_size);

	g_assert(frame == NULL);
	g_assert(frame_size == 0);

	total += nread;

	/* Try to read with just the open flag */
	nread = gsm0710_basic_extract_frame(basic_input + total,
						basic_frame_size + 1,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(nread == 0);
	g_assert(frame == NULL);

	/* Now read with the close flag as well */
	nread = gsm0710_basic_extract_frame(basic_input + total,
						basic_frame_size + 2,
						&dlc, &ctrl,
						&frame, &frame_size);

	/* Extracted the open flag + frame */
	g_assert(nread == basic_frame_size + 1);
	g_assert(frame_size == sizeof(basic_output));
	g_assert(memcmp(basic_output, frame, frame_size) == 0);

	total += nread;

	nread = gsm0710_basic_extract_frame(basic_input + total,
						sizeof(basic_input) - total,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(nread == (int)(sizeof(basic_input) - total - 1));
	g_assert(frame_size == sizeof(basic_output));
	g_assert(memcmp(basic_output, frame, frame_size) == 0);

	total += nread;

	nread = gsm0710_basic_extract_frame(basic_input + total,
						sizeof(basic_input) - total,
						&dlc, &ctrl,
						&frame, &frame_size);
	g_assert(nread == 0);

	total = 0;

	nread = gsm0710_basic_extract_frame(basic_input2 + total,
						sizeof(basic_input2) - total,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(nread == basic_garbage_size + basic_frame_size + 1);
	g_assert(frame_size == sizeof(basic_output));
	g_assert(memcmp(basic_output, frame, frame_size) == 0);

	total += nread;

	nread = gsm0710_basic_extract_frame(basic_input2 + total,
						sizeof(basic_input2) - total,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(frame_size == sizeof(basic_output));
	g_assert(memcmp(basic_output, frame, frame_size) == 0);

	total += nread;

	g_assert(total == sizeof(basic_input2) - 1);
}

static guint8 advanced_input[] =
	{ 0xFF, 0xFF, 0xFF, 0x7E, 0x07, 0xEF, 0x12, 0x34, 0x56, 0x05, 0x7E,
		0x07, 0xEF, 0x12, 0x34, 0x56, 0x05, 0x7E };

static guint8 advanced_input2[] =
	{ 0xFF, 0xFF, 0xFF, 0x7E, 0x07, 0xEF, 0x12, 0x34, 0x56, 0x05, 0x7E,
		0x07, 0xEF, 0x12, 0x34, 0x56, 0x05, 0x7E };

static int advanced_garbage_size = 3;
static int advanced_frame_size = 6;

static const guint8 advanced_output[] = { 0x12, 0x34, 0x56 };

static void test_extract_advanced(void)
{
	int total = 0;
	int nread;
	guint8 dlc;
	guint8 ctrl;
	guint8 *frame;
	int frame_size;

	frame = NULL;
	frame_size = 0;

	nread = gsm0710_advanced_extract_frame(advanced_input + total,
						advanced_garbage_size,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(frame == NULL);
	g_assert(frame_size == 0);

	total += nread;

	/* Try to read with just the open flag */
	nread = gsm0710_advanced_extract_frame(advanced_input + total,
						advanced_frame_size + 1,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(nread == 0);
	g_assert(frame == NULL);

	/* Now read with the close flag as well */
	nread = gsm0710_advanced_extract_frame(advanced_input + total,
						advanced_frame_size + 2,
						&dlc, &ctrl,
						&frame, &frame_size);

	/* Extracted the open flag + frame */
	g_assert(nread == advanced_frame_size + 1);
	g_assert(frame_size == sizeof(advanced_output));
	g_assert(memcmp(advanced_output, frame, frame_size) == 0);

	total += nread;

	nread = gsm0710_advanced_extract_frame(advanced_input + total,
						sizeof(advanced_input) - total,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(nread == (int)(sizeof(advanced_input) - total - 1));
	g_assert(frame_size == sizeof(advanced_output));
	g_assert(memcmp(advanced_output, frame, frame_size) == 0);

	total += nread;

	nread = gsm0710_advanced_extract_frame(advanced_input + total,
						sizeof(advanced_input) - total,
						&dlc, &ctrl,
						&frame, &frame_size);
	g_assert(nread == 0);

	total = 0;

	nread = gsm0710_advanced_extract_frame(advanced_input2 + total,
						sizeof(advanced_input2) - total,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(nread == advanced_garbage_size + advanced_frame_size + 1);
	g_assert(frame_size == sizeof(advanced_output));
	g_assert(memcmp(advanced_output, frame, frame_size) == 0);

	total += nread;

	nread = gsm0710_advanced_extract_frame(advanced_input2 + total,
						sizeof(advanced_input2) - total,
						&dlc, &ctrl,
						&frame, &frame_size);

	g_assert(frame_size == sizeof(advanced_output));
	g_assert(memcmp(advanced_output, frame, frame_size) == 0);

	total += nread;

	g_assert(total == sizeof(advanced_input2) - 1);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testmux/fill_basic", test_fill_basic);
	g_test_add_func("/testmux/fill_advanced", test_fill_advanced);
	g_test_add_func("/testmux/extract_basic", test_extract_basic);
	g_test_add_func("/testmux/extract_advanced", test_extract_advanced);
	g_test_add_func("/testmux/basic", test_basic);

	return g_test_run();
}
