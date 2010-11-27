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

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <gatchat.h>

#include <drivers/stemodem/caif_socket.h>
#include <drivers/stemodem/if_caif.h>

static GMainLoop *mainloop;

static int do_open(void)
{
	int fd;

	fd = open("/dev/chnlat11", O_RDWR);
	if (fd < 0) {
		g_printerr("Open of chnlat11 failed (%d)\n", errno);
		return -EIO;
	}

	return fd;
}

static int do_connect(void)
{
	struct sockaddr_caif addr;
	int sk, err;

	/* Create a CAIF socket for AT Service */
	sk = socket(AF_CAIF, SOCK_SEQPACKET, CAIFPROTO_AT);
	if (sk < 0) {
		g_printerr("CAIF socket creation failed (%d)\n", errno);
		return -EIO;
	}

	memset(&addr, 0, sizeof(addr));
	addr.family = AF_CAIF;
	addr.u.at.type = CAIF_ATTYPE_PLAIN;

	/* Connect to the AT Service at the modem */
	err = connect(sk, (struct sockaddr *) &addr, sizeof(addr));
	if (err < 0) {
		g_printerr("CAIF socket connect failed (%d)\n", errno);
		close(sk);
		return err;
	}

	return sk;
}

static void caif_debug(const char *str, void *data)
{
	g_print("%s\n", str);
}

static void caif_init(gboolean ok, GAtResult *result, gpointer data)
{
	GAtChat *chat = data;

	g_print("caif_init: %d\n", ok);

	if (ok == FALSE) {
		g_at_chat_unref(chat);
		g_main_loop_quit(mainloop);
		return;
	}

	g_at_chat_unref(chat);
	g_main_loop_quit(mainloop);
}

static void test_connect(gboolean use_socket)
{
	GIOChannel *io;
	GAtChat *chat;
	GAtSyntax *syntax;
	int fd;

	if (use_socket == TRUE)
		fd = do_connect();
	else
		fd = do_open();

	if (fd < 0)
		return;

	io = g_io_channel_unix_new(fd);
	g_io_channel_set_close_on_unref(io, TRUE);

	syntax = g_at_syntax_new_gsm_permissive();
	chat = g_at_chat_new_blocking(io, syntax);
	g_at_syntax_unref(syntax);

	g_io_channel_unref(io);

	if (chat == NULL) {
		g_printerr("Chat creation failed\n");
		return;
	}

	g_at_chat_set_debug(chat, caif_debug, NULL);
	g_at_chat_send(chat, "ATE0 +CMEE=1", NULL, caif_init, chat, NULL);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
	g_main_loop_unref(mainloop);
}

static void test_basic(void)
{
	if (g_test_trap_fork(60 * 1000 * 1000, 0) == TRUE) {
		test_connect(TRUE);
		exit(0);
	}

	g_test_trap_assert_passed();
	//g_test_trap_assert_stderr("failed");
}

static void test_chnlat(void)
{
	if (g_test_trap_fork(60 * 1000 * 1000, 0) == TRUE) {
		test_connect(FALSE);
		exit(0);
	}

	g_test_trap_assert_passed();
	//g_test_trap_assert_stderr("failed");
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testcaif/basic", test_basic);
	g_test_add_func("/testcaif/chnlat", test_chnlat);

	return g_test_run();
}
