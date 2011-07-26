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

#include <stdio.h>

#include <glib.h>

#include "gattty.h"
#include "gathdlc.h"

static gboolean option_debug = FALSE;
static gchar *option_device = NULL;

static GMainLoop *event_loop;

struct version_info {
	char comp_date[11];
	char comp_time[8];
	char rel_date[11];
	char rel_time[8];
	char model[8];
	guint8 scm;
	guint8 mob_cai_rev;
	guint8 mob_model;
	guint16 mob_firmware_rev;
	guint8 slot_cycle_index;
	guint8 msm_ver;
	guint8 unknown;
} __attribute__ ((packed));

static void parse_qcdm(const unsigned char *buf, gsize len)
{
	struct version_info *verinfo;
	char str[12];
	guint8 cmd = buf[0];

	switch (cmd) {
	case 0x00:
		g_print("==> Version information\n");
		verinfo = (struct version_info *) (buf + 1);
		snprintf(str, 12, "%s", verinfo->comp_date);
		g_print("Compiled Date: %s\n", str);
		snprintf(str, 9, "%s", verinfo->comp_time);
		g_print("Compiled Time: %s\n", str);
		snprintf(str, 12, "%s", verinfo->rel_date);
		g_print("Release Date: %s\n", str);
		snprintf(str, 9, "%s", verinfo->rel_time);
		g_print("Release Time: %s\n", str);
		snprintf(str, 9, "%s", verinfo->model);
		g_print("Model: %s\n", str);
		g_print("MSM version: %d\n", verinfo->msm_ver);
		break;
	case 0x13:
		g_print("==> Invalid command response\n");
		break;
	case 0x4b:
		g_print("==> Subsystem response\n");
		break;
	case 0x51:
		g_print("==> Features response\n");
		break;
	default:
		g_print("==> Unknown command 0x%02x\n", cmd);
		break;
	}
}

static void hdlc_debug(const char *str, void *data)
{
	g_print("%s: %s\n", (const char *) data, str);
}

static void hdlc_receive(const unsigned char *buf, gsize len, void *data)
{
	parse_qcdm(buf, len);
}

static void send_command(GAtHDLC *hdlc, guint8 cmd)
{
	unsigned char cmdbuf[1];

	cmdbuf[0] = cmd;

	g_at_hdlc_send(hdlc, cmdbuf, sizeof(cmdbuf));
}

static void send_subsys_command(GAtHDLC *hdlc, guint8 id, guint16 cmd)
{
	unsigned char cmdbuf[4];

	cmdbuf[0] = 0x4b;
	cmdbuf[1] = id;
	cmdbuf[2] = cmd & 0xff;
	cmdbuf[3] = cmd >> 8;

	g_at_hdlc_send(hdlc, cmdbuf, sizeof(cmdbuf));
}

static GOptionEntry options[] = {
	{ "debug", 'd', 0, G_OPTION_ARG_NONE, &option_debug,
						"Enable debugging" },
	{ "device", 'n', 0, G_OPTION_ARG_STRING, &option_device,
						"Specify device" },
	{ NULL },
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *err = NULL;
	GIOChannel *channel;
	GAtHDLC *hdlc;

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, &argc, &argv, &err) == FALSE) {
		if (err != NULL) {
			g_printerr("%s\n", err->message);
			g_error_free(err);
			return 1;
		}

		g_printerr("An unknown error occurred\n");
		return 1;
	}

	g_option_context_free(context);

	if (option_device == NULL)
		option_device = g_strdup("/dev/ttyUSB1");

	g_print("Device: %s\n", option_device);

	channel = g_at_tty_open_qcdm(option_device);
	if (channel == NULL) {
		g_printerr("Failed to open QCDM device\n");
		return 1;
	}

	event_loop = g_main_loop_new(NULL, FALSE);

	hdlc = g_at_hdlc_new(channel);

	g_io_channel_unref(channel);

	if (hdlc == NULL)
		return 1;

	if (option_debug == TRUE)
		g_at_hdlc_set_debug(hdlc, hdlc_debug, "HDLC");

	g_at_hdlc_set_xmit_accm(hdlc, 0);
	g_at_hdlc_set_recv_accm(hdlc, 0);

	g_at_hdlc_set_receive(hdlc, hdlc_receive, NULL);

	send_command(hdlc, 0x00);	/* Version info */
	send_command(hdlc, 0x51);	/* Features query */

	send_subsys_command(hdlc, 250, 7);	/* Novatel modem status */

	g_main_loop_run(event_loop);

	g_at_hdlc_unref(hdlc);

	g_main_loop_unref(event_loop);

	g_free(option_device);

	return 0;
}
