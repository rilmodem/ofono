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
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <glib.h>
#include <glib/gprintf.h>

#include "gsm0710.h"

static int at_command(struct gsm0710_context *ctx, const char *cmd)
{
	int len;

	g_print("sending: %s\n", cmd);

	len = write(ctx->fd, cmd, strlen(cmd));
	len = write(ctx->fd, "\r", 1);

	return 1;
}

static int do_write(struct gsm0710_context *ctx, const void *data, int size)
{
	int len;

	g_print("writing: %d bytes\n", size);

	len = write(ctx->fd, data, size);

	return 1;
}

static void debug_message(struct gsm0710_context *ctx, const char *msg)
{
	g_print("debug: %s\n", msg);
}

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

static void test_setup(void)
{
	struct gsm0710_context ctx;

	gsm0710_initialize(&ctx);

	ctx.fd = do_connect("127.0.0.1", 12345);
	if (ctx.fd < 0)
		return;

	ctx.at_command = at_command;
	ctx.write = do_write;
	ctx.debug_message = debug_message;

	gsm0710_startup(&ctx, 1);

	gsm0710_open_channel(&ctx, 1);

	sleep(10);

	gsm0710_shutdown(&ctx);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testmux/Setup", test_setup);

	return g_test_run();
}
