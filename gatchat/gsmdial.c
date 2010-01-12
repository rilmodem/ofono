/*
 *
 *  AT chat library with GLib integration
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

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/signalfd.h>

#include <glib.h>
#include <gatchat.h>
#include <gattty.h>

static const char *none_prefix[] = { NULL };
static const char *cgreg_prefix[] = { "+CGREG:", NULL };

static gchar *option_ip = NULL;
static gint option_port = 0;
static gchar *option_modem = NULL;
static gchar *option_control = NULL;
static gint option_cid = 0;
static gchar *option_apn = NULL;
static gint option_offmode = 0;
static gboolean option_legacy = FALSE;

static GAtChat *control;
static GAtChat *modem;
static GMainLoop *event_loop;

enum state {
	STATE_NONE = 0,
	STATE_REGISTERING,
	STATE_ACTIVATING
};

static int state = 0;

static void gsmdial_debug(const char *str, void *data)
{
	g_print("%s: %s\n", (const char *)data, str);
}

static gboolean quit_eventloop(gpointer user_data)
{
	g_main_loop_quit(event_loop);
	return FALSE;
}

static void power_down(gboolean ok, GAtResult *result, gpointer user_data)
{
	g_main_loop_quit(event_loop);
}

static gboolean signal_cb(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	static int terminated = 0;
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
	case SIGTERM:
		if (terminated == 0) {
			char buf[64];

			g_timeout_add_seconds(10, quit_eventloop, NULL);
			sprintf(buf, "AT+CFUN=%u", option_offmode);
			g_at_chat_send(control, buf, none_prefix,
					power_down, NULL, NULL);
		}

		terminated++;
		break;
	default:
		break;
	}

	return TRUE;
}

static gboolean at_util_parse_reg_unsolicited(GAtResult *result,
						const char *prefix, int *status,
						int *lac, int *ci, int *tech)
{
	GAtResultIter iter;
	int s;
	int l = -1, c = -1, t = -1;
	const char *str;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, prefix) == FALSE)
		return FALSE;

	if (g_at_result_iter_next_number(&iter, &s) == FALSE)
		return FALSE;

	if (g_at_result_iter_next_string(&iter, &str) == TRUE)
		l = strtol(str, NULL, 16);
	else
		goto out;

	if (g_at_result_iter_next_string(&iter, &str) == TRUE)
		c = strtol(str, NULL, 16);
	else
		goto out;

	g_at_result_iter_next_number(&iter, &t);

out:
	if (status)
		*status = s;

	if (lac)
		*lac = l;

	if (ci)
		*ci = c;

	if (tech)
		*tech = t;

	return TRUE;
}

static gboolean at_util_parse_reg(GAtResult *result, const char *prefix,
					int *mode, int *status,
					int *lac, int *ci, int *tech)
{
	GAtResultIter iter;
	int m, s;
	int l = -1, c = -1, t = -1;
	const char *str;

	g_at_result_iter_init(&iter, result);

	while (g_at_result_iter_next(&iter, prefix)) {
		g_at_result_iter_next_number(&iter, &m);

		/* Sometimes we get an unsolicited CREG/CGREG here, skip it */
		if (g_at_result_iter_next_number(&iter, &s) == FALSE)
			continue;

		if (g_at_result_iter_next_string(&iter, &str) == TRUE)
			l = strtol(str, NULL, 16);
		else
			goto out;

		if (g_at_result_iter_next_string(&iter, &str) == TRUE)
			c = strtol(str, NULL, 16);
		else
			goto out;

		g_at_result_iter_next_number(&iter, &t);

out:
		if (mode)
			*mode = m;

		if (status)
			*status = s;

		if (lac)
			*lac = l;

		if (ci)
			*ci = c;

		if (tech)
			*tech = t;

		return TRUE;
	}

	return FALSE;
}

static void at_cgact_up_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	char buf[64];

	if (!ok) {
		g_print("Error activating context\n");
		exit(1);
	}

	sprintf(buf, "AT+CGDATA=\"PPP\",%u", option_cid);

	g_at_chat_send(modem, buf, none_prefix, NULL, NULL, NULL);
}

static void at_cgdcont_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	char buf[64];

	if (!ok) {
		g_print("Unable to define context\n");
		exit(1);
	}

	if (option_legacy == TRUE) {
			sprintf(buf, "ATD*99***%u#", option_cid);
			g_at_chat_send(modem, buf, none_prefix,
					NULL, NULL, NULL);
	} else {
		sprintf(buf, "AT+CGACT=1,%u", option_cid);
		g_at_chat_send(control, buf, none_prefix,
				at_cgact_up_cb, NULL, NULL);
	}
}

static void setup_context(int status)
{
	char buf[1024];
	int len;

	state = STATE_ACTIVATING;

	g_print("Registered to GPRS network, roaming=%s\n",
			status == 5 ? "True" : "False");

	len = sprintf(buf, "AT+CGDCONT=%u,\"IP\"", option_cid);
	snprintf(buf + len, sizeof(buf) - len - 3, ",\"%s\"", option_apn);
	g_at_chat_send(control, buf, none_prefix, at_cgdcont_cb, NULL, NULL);
}

static void cgreg_notify(GAtResult *result, gpointer user_data)
{
	int status, lac, ci, tech;

	if (state != STATE_REGISTERING)
		return;

	if (at_util_parse_reg_unsolicited(result, "+CGREG:", &status,
				&lac, &ci, &tech) == FALSE)
		return;

	if (status != 1 && status != 5)
		return;

	setup_context(status);
}

static void cgreg_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	int status, lac, ci, tech;

	if (at_util_parse_reg(result, "+CGREG:", NULL, &status,
				&lac, &ci, &tech) == FALSE)
		return;

	if (status != 1 && status != 5)
		return;

	setup_context(status);
}

static void attached_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	if (!ok)
		return;

	g_at_chat_send(control, "AT+CGREG?", cgreg_prefix,
			cgreg_cb, NULL, NULL);
}

static void creg_notify(GAtResult *result, gpointer user_data)
{
	int status, lac, ci, tech;

	if (state != STATE_REGISTERING)
		return;

	if (at_util_parse_reg_unsolicited(result, "+CREG:", &status,
				&lac, &ci, &tech) == FALSE)
		return;

	if (status == 1 || status == 5) {
		g_print("Registered to network, roaming=%s\n",
				status == 5 ? "True" : "False");

		g_print("Activating gprs network...\n");
		g_at_chat_send(control, "AT+CGATT=1", none_prefix,
				attached_cb, NULL, NULL);
	}
}

static void register_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	if (!ok) {
		g_print("Couldn't register to network, exiting...\n");
		exit(1);
	}

	state = STATE_REGISTERING;
	g_print("Waiting for network registration...\n");
}

static void start_dial(gboolean ok, GAtResult *result, gpointer user_data)
{
	if (!ok) {
		g_print("Turning on the modem failed\n");
		exit(1);
	}

	g_at_chat_register(control, "+CREG:",
				creg_notify, FALSE, NULL, NULL);
	g_at_chat_register(control, "+CGREG:",
				cgreg_notify, FALSE, NULL, NULL);

	g_at_chat_send(control, "AT+CREG=2", none_prefix, NULL, NULL, NULL);
	g_at_chat_send(control, "AT+CGREG=2", none_prefix, NULL, NULL, NULL);

	g_at_chat_send(control, "AT+COPS=0", none_prefix,
			register_cb, NULL, NULL);
}

static int open_serial()
{
	GAtSyntax *syntax;
	GIOChannel *channel;

	channel = g_at_tty_open(option_control, NULL);
	if (channel == NULL)
		return -EIO;

	syntax = g_at_syntax_new_gsm_permissive();
	control = g_at_chat_new(channel, syntax);
	g_io_channel_unref(channel);
	g_at_syntax_unref(syntax);

	if (control == NULL)
		return -EIO;

	if (option_modem == NULL) {
		g_at_chat_ref(control);
		modem = control;
		g_at_chat_set_debug(control, gsmdial_debug, "");
	} else {
		g_at_chat_set_debug(control, gsmdial_debug, "Control");

		channel = g_at_tty_open(option_modem, NULL);
		if (channel == NULL)
			return -EIO;

		syntax = g_at_syntax_new_gsm_permissive();
		modem = g_at_chat_new(channel, syntax);
		g_io_channel_unref(channel);
		g_at_syntax_unref(syntax);

		if (modem == NULL)
			return -EIO;

		g_at_chat_set_debug(modem, gsmdial_debug, "Modem");
	}

	return 0;
}

static GOptionEntry options[] = {
	{ "ip", 'i', 0, G_OPTION_ARG_STRING, &option_ip,
				"Specify IP" },
	{ "port", 'p', 0, G_OPTION_ARG_INT, &option_port,
				"Specify IP Port" },
	{ "control", 'n', 0, G_OPTION_ARG_FILENAME, &option_control,
				"Specify Modem Control port" },
	{ "modem", 'm', 0, G_OPTION_ARG_FILENAME, &option_modem,
				"Specify Modem port (ppp), if not provided"
				" the control port will be used" },
	{ "cid", 'c', 0, G_OPTION_ARG_INT, &option_cid,
				"Specify CID to use" },
	{ "apn", 'a', 0, G_OPTION_ARG_STRING, &option_apn,
				"Specify APN" },
	{ "offmode", 'o', 0, G_OPTION_ARG_INT, &option_offmode,
				"Specify CFUN offmode" },
	{ "legacy", 'l', 0, G_OPTION_ARG_NONE, &option_legacy,
				"Use ATD*99***<cid>#" },
	{ NULL },
};

int main(int argc, char **argv)
{
	GOptionContext *context;
	GError *err = NULL;
	sigset_t mask;
	int signal_fd;
	GIOChannel *signal_io;
	int signal_source;

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

	if (option_control) {
		int ret;

		g_print("Control: %s\n", option_control);
		if (option_modem)
			g_print("Modem: %s\n", option_modem);

		ret = open_serial();
		g_free(option_control);
		g_free(option_modem);

		if (ret < 0)
			goto out;
	} else {
		g_print("IP: %s\n", option_ip);
		g_print("Port: %d\n", option_port);
		g_free(option_ip);

		goto out;
	}

	g_print("APN: %s\n", option_apn);
	g_print("CID: %d\n", option_cid);

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGUSR2);
	sigaddset(&mask, SIGPIPE);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("Can't set signal mask");
		return 1;
	}

	signal_fd = signalfd(-1, &mask, 0);
	if (signal_fd < 0) {
		perror("Can't create signal filedescriptor");
		return 1;
	}

	signal_io = g_io_channel_unix_new(signal_fd);
	g_io_channel_set_close_on_unref(signal_io, TRUE);
	signal_source = g_io_add_watch(signal_io,
			G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
			signal_cb, GINT_TO_POINTER(signal_fd));
	g_io_channel_unref(signal_io);

	event_loop = g_main_loop_new(NULL, FALSE);

	g_at_chat_send(control, "ATE0Q0V1", NULL, NULL, NULL, NULL);
	g_at_chat_send(control, "AT+CFUN=1", NULL, start_dial, NULL, NULL);

	g_main_loop_run(event_loop);
	g_source_remove(signal_source);
	g_main_loop_unref(event_loop);

out:
	g_at_chat_unref(control);
	g_at_chat_unref(modem);
	g_free(option_apn);

	return 0;
}
