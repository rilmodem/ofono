/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2015 Canonical Ltd.
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

#define _GNU_SOURCE
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <ofono/types.h>

#include <gril.h>

#include "rilmodem-test-server.h"

#define MAX_REQUEST_SIZE 4096
#define RIL_SERVER_SOCK_PATH    "/tmp/unittestril"

struct server_data {
	int server_sk;
	ConnectFunc connect_func;
	GIOChannel *server_io;
	char *sock_name;
	const struct rilmodem_test_data *rtd;
	void *user_data;
};

/* Warning: length is stored in network order */
struct rsp_hdr {
	uint32_t length;
	uint32_t unsolicited;
	uint32_t serial;
	uint32_t error;
};

static gboolean read_server(gpointer data)
{
	struct server_data *sd = data;
	GIOStatus status;
	gsize offset, rbytes, wbytes;
	gchar *buf, *bufp;
	uint32_t req_serial;
	struct rsp_hdr rsp;

	buf = g_malloc0(MAX_REQUEST_SIZE);

	status = g_io_channel_read_chars(sd->server_io, buf, MAX_REQUEST_SIZE,
								&rbytes, NULL);
	g_assert(status == G_IO_STATUS_NORMAL);
	g_assert(rbytes == sd->rtd->req_size);

	/* validate len, and request_id */
	g_assert(!memcmp(buf, sd->rtd->req_data, (sizeof(uint32_t) * 2)));

	/*
	 * header: size (uint32), reqid (uin32), serial (uint32)
	 * header size == 16 ( excludes sizeof(size) )
	 */

	/* advance past request_no */
	bufp = buf + (sizeof(uint32_t) * 2);

	req_serial = (uint32_t) *bufp;

	/* advance past serial_no */
	bufp += sizeof(uint32_t);

	/* validate the rest of the parcel... */
	offset = (sizeof(uint32_t) * 3);
	g_assert(!memcmp(bufp, sd->rtd->req_data + offset,
						sd->rtd->req_size - offset));

	/* Length does not include the length field. Network order. */
	rsp.length = htonl(sizeof(rsp) - sizeof(rsp.length) +
							sd->rtd->rsp_size);
	rsp.unsolicited = 0;
	rsp.serial = req_serial;
	rsp.error = sd->rtd->rsp_error;

	/* copy header */
	memcpy(buf, &rsp, sizeof(rsp));

	if (sd->rtd->rsp_size) {
		bufp = buf + sizeof(rsp);

		memcpy(bufp, sd->rtd->rsp_data, sd->rtd->rsp_size);
	}

	status = g_io_channel_write_chars(sd->server_io,
					buf,
					sizeof(rsp) + sd->rtd->rsp_size,
					&wbytes, NULL);

	/* FIXME: assert wbytes is correct */

	g_assert(status == G_IO_STATUS_NORMAL);

	g_free(buf);
	g_io_channel_unref(sd->server_io);

	return FALSE;
}

static gboolean on_socket_connected(GIOChannel *chan, GIOCondition cond,
								gpointer data)
{
	struct server_data *sd = data;
	struct sockaddr saddr;
	unsigned int len = sizeof(saddr);
	int fd;
	GIOStatus status;

	g_assert(cond == G_IO_IN);

	fd = accept(sd->server_sk, &saddr, &len);
	g_assert(fd != -1);

	sd->server_io = g_io_channel_unix_new(fd);
	g_assert(sd->server_io != NULL);

	status = g_io_channel_set_encoding(sd->server_io, NULL, NULL);
	g_assert(status == G_IO_STATUS_NORMAL);

	g_io_channel_set_buffered(sd->server_io, FALSE);
	g_io_channel_set_close_on_unref(sd->server_io, TRUE);

	if (sd->connect_func)
		sd->connect_func(sd->user_data);

	if (sd->rtd->unsol_test == FALSE)
		g_idle_add(read_server, sd);

	return FALSE;
}

void rilmodem_test_server_close(struct server_data *sd)
{
	g_assert(sd->server_sk);
	close(sd->server_sk);
	remove(sd->sock_name);
	g_free(sd->sock_name);
	g_free(sd);
}

struct server_data *rilmodem_test_server_create(ConnectFunc connect,
				const struct rilmodem_test_data *test_data,
				void *data)
{
	GIOChannel *io;
	struct sockaddr_un addr;
	int retval;
	struct server_data *sd;

	sd = g_new0(struct server_data, 1);

	sd->connect_func = connect;
	sd->user_data = data;
	sd->rtd = test_data;

	sd->server_sk = socket(AF_UNIX, SOCK_STREAM, 0);
	g_assert(sd->server_sk);

	sd->sock_name =
		g_strdup_printf(RIL_SERVER_SOCK_PATH"%u", (unsigned) getpid());

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sd->sock_name, sizeof(addr.sun_path) - 1);

	/* Unlink any existing socket for this session */
	unlink(addr.sun_path);

	retval = bind(sd->server_sk, (struct sockaddr *) &addr, sizeof(addr));
	g_assert(retval >= 0);

	retval = listen(sd->server_sk, 0);
	g_assert(retval >= 0);

	io = g_io_channel_unix_new(sd->server_sk);
	g_assert(io != NULL);

	g_io_channel_set_close_on_unref(io, TRUE);
	g_io_add_watch_full(io,	G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, sd, NULL);

	g_io_channel_unref(io);

	return sd;
}

void rilmodem_test_server_write(struct server_data *sd,
						const unsigned char *buf,
						const size_t buf_len)
{
	GIOStatus status;
	gsize wbytes;

	status = g_io_channel_write_chars(sd->server_io,
					(const char *) buf,
					buf_len,
					&wbytes, NULL);

	g_assert(status == G_IO_STATUS_NORMAL);

	status = g_io_channel_flush(sd->server_io, NULL);

	g_assert(status == G_IO_STATUS_NORMAL);
}

const char *rilmodem_test_get_socket_name(struct server_data *sd)
{
	return sd->sock_name;
}
