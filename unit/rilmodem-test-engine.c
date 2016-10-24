/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016 Canonical Ltd.
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

#include "rilmodem-test-engine.h"

#define MAX_REQUEST_SIZE 4096
#define RIL_SERVER_SOCK_PATH "/tmp/unittestril"

static GMainLoop *mainloop;

struct engine_data {
	int server_sk;
	int connected_sk;
	guint connection_watch;
	rilmodem_test_engine_cb_t connect_func;
	GIOChannel *server_io;
	char *sock_name;
	struct rilmodem_test_data rtd;
	int step_i;
	void *user_data;
};

static void send_parcel(struct engine_data *ed)
{
	GIOStatus status;
	gsize wbytes;
	const struct rilmodem_test_step *step = &ed->rtd.steps[ed->step_i];

	status = g_io_channel_write_chars(ed->server_io,
						step->parcel_data,
						step->parcel_size,
						&wbytes, NULL);

	g_assert(wbytes == step->parcel_size);
	g_assert(status == G_IO_STATUS_NORMAL);

	status = g_io_channel_flush(ed->server_io, NULL);
	g_assert(status == G_IO_STATUS_NORMAL);

	rilmodem_test_engine_next_step(ed);
}

static gboolean on_rx_data(GIOChannel *chan, GIOCondition cond, gpointer data)
{
	struct engine_data *ed = data;
	GIOStatus status;
	gsize rbytes;
	gchar *buf;
	const struct rilmodem_test_step *step;

	/* We have closed the socket */
	if (cond == G_IO_NVAL)
		return FALSE;

	buf = g_malloc0(MAX_REQUEST_SIZE);

	status = g_io_channel_read_chars(ed->server_io, buf, MAX_REQUEST_SIZE,
								&rbytes, NULL);
	g_assert(status == G_IO_STATUS_NORMAL);

	/* Check this is the expected step */
	step = &ed->rtd.steps[ed->step_i];
	g_assert(step->type == TST_EVENT_RECEIVE);

	g_assert(rbytes == step->parcel_size);

	/* validate received parcel */
	g_assert(!memcmp(buf, step->parcel_data, rbytes));

	rilmodem_test_engine_next_step(ed);

	return TRUE;
}

static gboolean on_socket_connected(GIOChannel *chan, GIOCondition cond,
								gpointer data)
{
	struct engine_data *ed = data;
	struct sockaddr saddr;
	unsigned int len = sizeof(saddr);
	GIOStatus status;

	g_assert(cond == G_IO_IN);

	ed->connected_sk = accept(ed->server_sk, &saddr, &len);
	g_assert(ed->connected_sk != -1);

	ed->server_io = g_io_channel_unix_new(ed->connected_sk);
	g_assert(ed->server_io != NULL);

	status = g_io_channel_set_encoding(ed->server_io, NULL, NULL);
	g_assert(status == G_IO_STATUS_NORMAL);

	g_io_channel_set_buffered(ed->server_io, FALSE);
	g_io_channel_set_close_on_unref(ed->server_io, TRUE);

	if (ed->connect_func)
		ed->connect_func(ed->user_data);

	ed->connection_watch =
		g_io_add_watch_full(ed->server_io, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_rx_data, ed, NULL);
	g_io_channel_unref(ed->server_io);

	return FALSE;
}

void rilmodem_test_engine_remove(struct engine_data *ed)
{
	if (ed->connection_watch)
		g_source_remove(ed->connection_watch);

	g_assert(ed->server_sk);
	close(ed->server_sk);
	remove(ed->sock_name);
	g_free(ed->sock_name);
	g_free(ed);
}

struct engine_data *rilmodem_test_engine_create(
				rilmodem_test_engine_cb_t connect,
				const struct rilmodem_test_data *test_data,
				void *data)
{
	GIOChannel *io;
	struct sockaddr_un addr;
	int retval;
	struct engine_data *ed;

	ed = g_new0(struct engine_data, 1);

	ed->connect_func = connect;
	ed->user_data = data;
	ed->rtd = *test_data;

	ed->server_sk = socket(AF_UNIX, SOCK_STREAM, 0);
	g_assert(ed->server_sk);

	ed->sock_name =
		g_strdup_printf(RIL_SERVER_SOCK_PATH"%u", (unsigned) getpid());

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, ed->sock_name, sizeof(addr.sun_path) - 1);

	/* Unlink any existing socket for this session */
	unlink(addr.sun_path);

	retval = bind(ed->server_sk, (struct sockaddr *) &addr, sizeof(addr));
	g_assert(retval >= 0);

	retval = listen(ed->server_sk, 0);
	g_assert(retval >= 0);

	io = g_io_channel_unix_new(ed->server_sk);
	g_assert(io != NULL);

	g_io_channel_set_close_on_unref(io, TRUE);
	g_io_add_watch_full(io,	G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, ed, NULL);

	g_io_channel_unref(io);

	return ed;
}

void rilmodem_test_engine_write_socket(struct engine_data *ed,
						const unsigned char *buf,
						const size_t buf_len)
{
	GIOStatus status;
	gsize wbytes;

	status = g_io_channel_write_chars(ed->server_io,
					(const char *) buf,
					buf_len,
					&wbytes, NULL);

	g_assert(status == G_IO_STATUS_NORMAL);

	status = g_io_channel_flush(ed->server_io, NULL);

	g_assert(status == G_IO_STATUS_NORMAL);
}

const char *rilmodem_test_engine_get_socket_name(struct engine_data *ed)
{
	return ed->sock_name;
}

static gboolean action_call(gpointer data)
{
	struct engine_data *ed = data;
	const struct rilmodem_test_step *step;

	step = &ed->rtd.steps[ed->step_i];

	step->call_action(ed->user_data);

	return FALSE;
}

void rilmodem_test_engine_next_step(struct engine_data *ed)
{
	const struct rilmodem_test_step *step;

	ed->step_i++;

	if (ed->step_i >= ed->rtd.num_steps) {
		/* Finish the test */
		g_main_loop_quit(mainloop);
		return;
	}

	step = &ed->rtd.steps[ed->step_i];

	/* If next step is an action, execute it */
	switch (step->type) {
	case TST_ACTION_SEND:
		send_parcel(ed);
		break;
	case TST_ACTION_CALL:
		g_idle_add(action_call, ed);
		break;
	case TST_EVENT_RECEIVE:
	case TST_EVENT_CALL:
		break;
	};
}

const struct rilmodem_test_step *rilmodem_test_engine_get_current_step(
							struct engine_data *ed)
{
	const struct rilmodem_test_step *step = &ed->rtd.steps[ed->step_i];

	return step;
}

void rilmodem_test_engine_start(struct engine_data *ed)
{
	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
	g_main_loop_unref(mainloop);
}
