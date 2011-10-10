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

#include <unistd.h>
#include <stdio.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <errno.h>

#include <glib.h>
#include <utmp.h>
#include <pty.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "gatserver.h"
#include "gatppp.h"
#include "ringbuffer.h"

#define DEFAULT_TCP_PORT 12346
#define DEFAULT_SOCK_PATH "./server_sock"
#define IFCONFIG_PATH "/sbin/ifconfig"

static gboolean data_mode = FALSE;
static int modem_mode = 0;
static int modem_creg = 0;
static int modem_cgreg = 0;
static int network_status = 4;
static int network_attach = 0;

struct sock_server{
	int server_sock;
};

static GMainLoop *mainloop;
static GAtServer *server;
static GAtPPP *ppp;
unsigned int server_watch;

static gboolean server_cleanup(void)
{
	if (server_watch)
		g_source_remove(server_watch);

	if (ppp) {
		g_at_ppp_unref(ppp);
		ppp = NULL;
	}

	g_at_server_unref(server);
	server = NULL;

	unlink(DEFAULT_SOCK_PATH);

	g_main_loop_quit(mainloop);

	return FALSE;
}

static void server_debug(const char *str, void *data)
{
	g_print("%s: %s\n", (char *) data, str);
}

static gboolean execute(const char *cmd)
{
	int status;

	status = system(cmd);
	if (status < 0) {
		g_print("Failed to execute command: %s\n", strerror(errno));
		return FALSE;
	}

	return TRUE;
}

static void ppp_connect(const char *iface, const char *local, const char *peer,
			const char *dns1, const char *dns2,
			gpointer user)
{
	char buf[512];

	g_print("Network Device: %s\n", iface);
	g_print("IP Address: %s\n", local);
	g_print("Peer IP Address: %s\n", peer);
	g_print("Primary DNS Server: %s\n", dns1);
	g_print("Secondary DNS Server: %s\n", dns2);

	snprintf(buf, sizeof(buf), "%s %s up", IFCONFIG_PATH, iface);
	execute(buf);

	snprintf(buf, sizeof(buf), "%s %s %s pointopoint %s", IFCONFIG_PATH,
				iface, local, peer);
	execute(buf);

	snprintf(buf, sizeof(buf), "echo 1 > /proc/sys/net/ipv4/ip_forward");
	execute(buf);
}

static void ppp_disconnect(GAtPPPDisconnectReason reason, gpointer user)
{
	GAtServer *server = user;

	g_print("PPP Link down: %d\n", reason);

	g_at_ppp_unref(ppp);
	ppp = NULL;

	if (reason == G_AT_PPP_REASON_LINK_DEAD) {
		g_at_server_unref(server);
		server = NULL;
		return;
	}

	g_at_server_resume(server);
	g_at_server_set_debug(server, server_debug, "Server");

	g_at_server_send_final(server, G_AT_SERVER_RESULT_NO_CARRIER);
	data_mode = FALSE;
}

static void open_ppp(gpointer user)
{
	GAtIO *io = g_at_server_get_io(server);

	g_at_server_suspend(server);
	g_at_ppp_listen(ppp, io);
}

static gboolean setup_ppp(GAtServer *server)
{
	/* open ppp */
	ppp = g_at_ppp_server_new("192.168.1.1");
	if (ppp == NULL)
		return FALSE;

	g_at_ppp_set_debug(ppp, server_debug, "PPP");

	g_at_ppp_set_credentials(ppp, "", "");

	/* set connect and disconnect callbacks */
	g_at_ppp_set_connect_function(ppp, ppp_connect, server);
	g_at_ppp_set_disconnect_function(ppp, ppp_disconnect, server);
	g_at_ppp_set_server_info(ppp, "192.168.1.2",
					"10.10.10.10", "10.10.10.11");

	return TRUE;
}

static void cgmi_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		g_at_server_send_info(server, "oFono", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void cgmm_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		g_at_server_send_info(server, "oFono pre-1.0", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void cgmr_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[256];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		sprintf(buf, "oFono pre-1.0 version: %s", VERSION);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void cgsn_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		g_at_server_send_info(server, "123456789", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static gboolean send_ok(gpointer user)
{
	GAtServer *server = user;

	g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

	return FALSE;
}

static void cfun_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[12];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_info(server, "+CFUN: (0-1)", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		snprintf(buf, sizeof(buf), "+CFUN: %d", modem_mode);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int mode;

		g_at_result_iter_init(&iter, cmd);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &mode) == FALSE)
			goto error;

		if (mode != 0 && mode != 1)
			goto error;

		if (modem_mode == mode) {
			g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
			break;
		}

		modem_mode = mode;
		g_timeout_add_seconds(1, send_ok, server);
		break;
	}
	default:
		goto error;
	};

	return;

error:
	g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
}

static void cpin_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	if (modem_mode == 0) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_info(server, "+CPIN: (READY)", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_info(server, "+CPIN: READY", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static gboolean do_netreg(gpointer user)
{
	GAtServer *server = user;
	char buf[32];

	if (data_mode)
		return FALSE;

	network_status = 1;

	switch (modem_creg) {
	case 1:
	case 2:
		snprintf(buf, sizeof(buf), "+CREG: %d", network_status);
		g_at_server_send_unsolicited(server, buf);
		break;
	}

	return FALSE;
}

static void cops_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	if (modem_mode == 0) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_timeout_add_seconds(3, send_ok, server);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_info(server, "+COPS: 0", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtServerResult result;
		GAtResultIter iter;
		int mode;

		g_at_result_iter_init(&iter, cmd);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &mode) == TRUE) {
			if (mode == 0) {
				g_timeout_add_seconds(2, do_netreg, server);
				result = G_AT_SERVER_RESULT_OK;
			} else
				result = G_AT_SERVER_RESULT_ERROR;
		} else
			result = G_AT_SERVER_RESULT_ERROR;

		g_at_server_send_final(server, result);
		break;
	}
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void creg_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[20];

	if (modem_mode == 0) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_info(server, "+CREG: (0-2)", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		snprintf(buf, sizeof(buf), "+CREG: %d,%d",
						modem_creg, network_status);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int mode;

		g_at_result_iter_init(&iter, cmd);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &mode) == FALSE)
			goto error;

		if (mode != 0 && mode != 1 && mode != 2)
			goto error;

		modem_creg = mode;
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	}
	default:
		goto error;
	};

	return;

error:
	g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
}

static void cgreg_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[20];

	if (modem_mode == 0) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_info(server, "+CGREG: (0-2)", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		snprintf(buf, sizeof(buf), "+CGREG: %d,%d",
						modem_cgreg, network_status);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int mode;

		g_at_result_iter_init(&iter, cmd);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &mode) == FALSE)
			goto error;

		if (mode != 0 && mode != 1 && mode != 2)
			goto error;

		modem_cgreg = mode;
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	}
	default:
		goto error;
	};

	return;

error:
	g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
}

static void cgatt_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[12];

	if (modem_mode == 0) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_info(server, "+CGATT: (0-1)", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		snprintf(buf, sizeof(buf), "+CGATT: %d", network_attach);
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int mode;

		g_at_result_iter_init(&iter, cmd);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &mode) == FALSE)
			goto error;

		if (mode != 0 && mode != 1)
			goto error;

		if (network_attach == mode) {
			g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
			break;
		}

		network_attach = mode;
		g_timeout_add_seconds(1, send_ok, server);
		break;
	}
	default:
		goto error;
	};

	return;

error:
	g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
}

static void cgdata_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	GAtIO *io;

	if (modem_mode == 0) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		if (setup_ppp(server) == FALSE)
			goto error;

		g_at_server_send_intermediate(server, "CONNECT");
		data_mode = TRUE;

		io = g_at_server_get_io(server);
		g_at_io_set_write_done(io, open_ppp, server);
		break;
	default:
error:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cgdcont_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	if (modem_mode == 0) {
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cimi_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		g_at_server_send_info(server, "246813579", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void csms_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_info(server, "+CSMS: 0,1,1,1", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_info(server, "+CSMS: (0)", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cmgf_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_info(server, "+CMGF: 0", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_info(server, "+CMGF: (0,1)", TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cpms_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[2048];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "+CPMS: (\"SM\",\"ME\"),(\"SM\",\"ME\"),(\"SM\")");
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cnmi_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[2048];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "+CNMI: (0,1,2,3),(0,1),(0,1,2),(0),(0,1)");
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cscs_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[2048];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "+CSCS: \"GSM\",\"IRA\",\"UCS2\"");
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cmgl_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cpbs_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	char buf[2048];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "+CPBS: (\"FD\",\"SM\",\"SN\")");
		g_at_server_send_info(server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	default:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void dial_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *cmd, gpointer user)
{
	GAtResultIter iter;
	const char *dial_str;
	char c;

	if (type != G_AT_SERVER_REQUEST_TYPE_SET)
		goto error;

	g_at_result_iter_init(&iter, cmd);
	g_at_result_iter_next(&iter, "");

	dial_str = g_at_result_iter_raw_line(&iter);
	if (dial_str == NULL)
		goto error;

	g_print("dial call %s\n", dial_str);

	c = *dial_str;
	if (c == '*' || c == '#' || c == 'T' || c == 't') {
		GAtIO *io = g_at_server_get_io(server);

		if (setup_ppp(server) == FALSE)
			goto error;

		g_at_server_send_intermediate(server, "CONNECT");
		data_mode = TRUE;
		g_at_io_set_write_done(io, open_ppp, server);
	}

	return;

error:
	g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
}

static void add_handler(GAtServer *server)
{
	g_at_server_set_debug(server, server_debug, "Server");

	g_at_server_register(server, "+CGMI",    cgmi_cb,    NULL, NULL);
	g_at_server_register(server, "+CGMM",    cgmm_cb,    NULL, NULL);
	g_at_server_register(server, "+CGMR",    cgmr_cb,    NULL, NULL);
	g_at_server_register(server, "+CGSN",    cgsn_cb,    NULL, NULL);
	g_at_server_register(server, "+CFUN",    cfun_cb,    NULL, NULL);
	g_at_server_register(server, "+CPIN",    cpin_cb,    NULL, NULL);
	g_at_server_register(server, "+COPS",    cops_cb,    NULL, NULL);
	g_at_server_register(server, "+CREG",    creg_cb,    NULL, NULL);
	g_at_server_register(server, "+CGREG",   cgreg_cb,   NULL, NULL);
	g_at_server_register(server, "+CGATT",   cgatt_cb,   NULL, NULL);
	g_at_server_register(server, "+CGDATA",  cgdata_cb,  NULL, NULL);
	g_at_server_register(server, "+CGDCONT", cgdcont_cb, NULL, NULL);
	g_at_server_register(server, "+CIMI",    cimi_cb,    NULL, NULL);
	g_at_server_register(server, "+CSMS",    csms_cb,    NULL, NULL);
	g_at_server_register(server, "+CMGF",    cmgf_cb,    NULL, NULL);
	g_at_server_register(server, "+CPMS",    cpms_cb,    NULL, NULL);
	g_at_server_register(server, "+CNMI",    cnmi_cb,    NULL, NULL);
	g_at_server_register(server, "+CSCS",    cscs_cb,    NULL, NULL);
	g_at_server_register(server, "+CMGL",    cmgl_cb,    NULL, NULL);
	g_at_server_register(server, "+CPBS",    cpbs_cb,    NULL, NULL);
	g_at_server_register(server, "D",        dial_cb,    NULL, NULL);
}

static void server_destroy(gpointer user)
{
	struct sock_server *data = user;

	g_free(data);
}

static void set_raw_mode(int fd)
{
	struct termios ti;

	memset(&ti, 0, sizeof(ti));
	tcgetattr(fd, &ti);
	tcflush(fd, TCIOFLUSH);
	cfmakeraw(&ti);
	tcsetattr(fd, TCSANOW, &ti);
}

static gboolean create_tty(const char *modem_path)
{
	int master, slave;
	char pty_name[256];
	GIOChannel *server_io;

	if (modem_path == NULL)
		return FALSE;

	if (openpty(&master, &slave, pty_name, NULL, NULL) < 0)
		return FALSE;

	set_raw_mode(slave);

	g_print("new pty is created at %s\n", pty_name);

	server_io = g_io_channel_unix_new(master);

	server = g_at_server_new(server_io);
	if (server == NULL) {
		g_io_channel_shutdown(server_io, FALSE, NULL);
		g_io_channel_unref(server_io);

		return FALSE;
	}

	g_io_channel_unref(server_io);

	return TRUE;
}

static gboolean on_socket_connected(GIOChannel *chan, GIOCondition cond,
							gpointer user)
{
	struct sockaddr saddr;
	unsigned int len = sizeof(saddr);
	int fd;
	GIOChannel *client_io = NULL;
	struct sock_server *data = user;

	if (cond != G_IO_IN)
		goto error;

	fd = accept(data->server_sock, &saddr, &len);
	if (fd == -1)
		goto error;

	client_io = g_io_channel_unix_new(fd);

	server = g_at_server_new(client_io);
	g_io_channel_unref(client_io);

	if (server == NULL)
		goto error;

	add_handler(server);

	return TRUE;

error:
	g_free(data);

	return FALSE;
}

static struct sock_server *socket_common(int sk, struct sockaddr *addr,
						const char *modem_path)
{
	struct sock_server *sock;
	int reuseaddr = 1;

	setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(reuseaddr));

	if (bind(sk, addr, sizeof(struct sockaddr)) < 0) {
		g_print("Can't bind socket: %s (%d)", strerror(errno), errno);

		close(sk);

		return NULL;
	}

	if (listen(sk, 1) < 0) {
		g_print("Can't listen on socket: %s (%d)",
						strerror(errno), errno);

		close(sk);

		return NULL;
	}

	sock = g_try_new0(struct sock_server, 1);
	if (sock == NULL)
		return FALSE;

	sock->server_sock = sk;

	return sock;
}

static gboolean create_tcp(const char *modem_path, int port)
{
	struct sockaddr_in addr;
	int sk;
	struct sock_server *server;
	GIOChannel *server_io;

	if (modem_path == NULL)
		return FALSE;

	sk = socket(PF_INET, SOCK_STREAM, 0);
	if (sk < 0) {
		g_print("Can't create tcp/ip socket: %s (%d)\n",
						strerror(errno), errno);
		return FALSE;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(port);

	server = socket_common(sk, (struct sockaddr *) &addr, modem_path);
	if (server == NULL)
		return FALSE;

	g_print("new tcp is created at tcp port %d\n", port);

	server_io = g_io_channel_unix_new(sk);

	g_io_channel_set_close_on_unref(server_io, TRUE);

	server_watch = g_io_add_watch_full(server_io,
				G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, server, server_destroy);

	g_io_channel_unref(server_io);

	return TRUE;
}

static gboolean create_unix(const char *modem_path, const char *sock_path)
{
	struct sockaddr_un addr;
	int sk;
	struct sock_server *server;
	GIOChannel *server_io;

	if (modem_path == NULL)
		return FALSE;

	sk = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sk < 0) {
		g_print("Can't create unix socket: %s (%d)\n",
						strerror(errno), errno);

		return FALSE;
	}

	memset(&addr, 0, sizeof(addr));

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	/* Unlink any existing socket for this session */
	unlink(addr.sun_path);

	server = socket_common(sk, (struct sockaddr *) &addr, modem_path);
	if (server == NULL)
		return FALSE;

	g_print("new unix socket is created at %s\n", sock_path);

	server_io = g_io_channel_unix_new(sk);

	g_io_channel_set_close_on_unref(server_io, TRUE);

	server_watch = g_io_add_watch_full(server_io,
				G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, server, server_destroy);

	g_io_channel_unref(server_io);

	return TRUE;
}

static void test_server(int type)
{
	switch (type) {
	case 0:
		if (create_tty("/phonesim1") == FALSE)
			exit(1);

		add_handler(server);
		break;
	case 1:
		if (create_tcp("/phonesim1", DEFAULT_TCP_PORT) == FALSE)
			exit(1);
		break;
	case 2:
		if (create_unix("/phonesim1", DEFAULT_SOCK_PATH) == FALSE)
			exit(1);
		break;
	}
}

static gboolean signal_cb(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	int signal_fd = GPOINTER_TO_INT(data);
	struct signalfd_siginfo si;
	ssize_t res;

	if (cond & (G_IO_NVAL | G_IO_ERR))
		return FALSE;

	res = read(signal_fd, &si, sizeof(si));
	if (res != sizeof(si))
		return FALSE;

	switch (si.ssi_signo) {
	case SIGINT:
		server_cleanup();
		break;
	case SIGTERM:
		server_cleanup();
		break;
	default:
		break;
	}

	return TRUE;
}

static int create_signal_io(void)
{
	sigset_t mask;
	GIOChannel *signal_io;
	int signal_fd, signal_source;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		g_error("Can't set signal mask");
		return 1;
	}

	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd < 0) {
		g_error("Can't create signal filedescriptor");
		return 1;
	}

	signal_io = g_io_channel_unix_new(signal_fd);

	g_io_channel_set_close_on_unref(signal_io, TRUE);

	signal_source = g_io_add_watch(signal_io,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			signal_cb, GINT_TO_POINTER(signal_fd));

	g_io_channel_unref(signal_io);

	return signal_source;
}

static void usage(void)
{
	g_print("test-server - AT Server testing\n"
		"Usage:\n");
	g_print("\ttest-server [-t type]\n");
	g_print("Types:\n"
		"\t0: Pseudo TTY port (default)\n"
		"\t1: TCP sock at port 12346)\n"
		"\t2: Unix sock at ./server_sock\n");
}

int main(int argc, char **argv)
{
	int opt, signal_source;
	int type = 0;

	while ((opt = getopt(argc, argv, "ht:")) != EOF) {
		switch (opt) {
		case 't':
			type = atoi(optarg);
			break;
		case 'h':
			usage();
			exit(1);
			break;
		default:
			break;
		}
	}

	test_server(type);

	signal_source = create_signal_io();

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);

	g_main_loop_unref(mainloop);

	g_source_remove(signal_source);

	return 0;
}
