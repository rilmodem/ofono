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

#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <gatchat.h>
#include <gatsyntax.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/call-barring.h>
#include <ofono/call-forwarding.h>
#include <ofono/call-meter.h>
#include <ofono/call-settings.h>
#include <ofono/devinfo.h>
#include <ofono/message-waiting.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/sim.h>
#include <ofono/sms.h>
#include <ofono/ssn.h>
#include <ofono/ussd.h>
#include <ofono/voicecall.h>

static GSList *g_modems = NULL;

enum transport_type {
	TRANSPORT_TYPE_TCP = 0,
	TRANSPORT_TYPE_UNIX = 1,
	TRANSPORT_TYPE_TTY = 2,
};

struct generic_at_data {
	enum transport_type type;
	union {
		struct sockaddr_in tcp;
		struct sockaddr_un un;
		char *tty;
	};

	char *timeout_command;
	int timeout_interval;
	char *init_string;
	GAtChat *chat;
	GIOChannel *io;
	unsigned int timeout_watcher;
};

static void generic_at_data_free(struct generic_at_data *d)
{
	if (d->type == TRANSPORT_TYPE_TTY)
		g_free(d->tty);

	g_free(d->timeout_command);
	g_free(d->init_string);

	if (d->chat)
		g_at_chat_unref(d->chat);

	g_free(d);
}

static struct generic_at_data *parse_modem(GKeyFile *keyfile, const char *group)
{
	struct generic_at_data *d;
	char *type;

	d = g_new0(struct generic_at_data, 1);

	type = g_key_file_get_string(keyfile, group, "Type", NULL);

	if (type == NULL)
		goto error;

	if (!strcmp(type, "tcp"))
		d->type = TRANSPORT_TYPE_TCP;
	else if (!strcmp(type, "unix"))
		d->type = TRANSPORT_TYPE_UNIX;
	else if (!strcmp(type, "tty"))
		d->type = TRANSPORT_TYPE_TTY;
	else
		goto error;

	g_free(type);

	switch (d->type) {
	case TRANSPORT_TYPE_TCP:
	{
		in_addr_t inetaddr;
		char *address;
		int port;

		address = g_key_file_get_string(keyfile, group, "Address", NULL);

		if (address == NULL)
			goto error;

		inetaddr = inet_addr(address);

		g_free(address);

		if (inetaddr == INADDR_NONE)
			goto error;

		port = g_key_file_get_integer(keyfile, group, "Port", NULL);

		if (port <= 0 || port > 0xffff)
			goto error;

		d->tcp.sin_family = AF_INET;
		d->tcp.sin_addr.s_addr = inetaddr;
		d->tcp.sin_port = htons(port);

		break;
	}

	case TRANSPORT_TYPE_UNIX:
	{
		char *path;

		path = g_key_file_get_string(keyfile, group, "Address", NULL);

		if (strlen(path) >= 108) {
			g_free(path);
			goto error;
		}

		d->un.sun_family = PF_UNIX;

		if (strncmp("x00", path, 3) == 0)
			strcpy(d->un.sun_path + 1, path + 3);
		else
			strcpy(d->un.sun_path, path);

		g_free(path);

		break;
	}

	case TRANSPORT_TYPE_TTY:
	{
		char *node;

		node = g_key_file_get_string(keyfile, group, "Device", NULL);

		d->tty = node;

		break;
	}

	default:
		break;
	}

	d->init_string = g_key_file_get_string(keyfile, group,
						"InitString", NULL);

	d->timeout_interval = g_key_file_get_integer(keyfile, group,
						"TimeoutInterval", NULL);

	if (d->timeout_interval < 0 || d->timeout_interval > 3600)
		goto error;

	d->timeout_command = g_key_file_get_string(keyfile, group,
							"TimeoutCommand", NULL);

	return d;

error:
	generic_at_data_free(d);
	return NULL;
}

static void parse_config(const char *file)
{
	GError *err = NULL;
	GKeyFile *keyfile;
	char **modems;
	int i;

	keyfile = g_key_file_new();

	g_key_file_set_list_separator(keyfile, ',');

	if (!g_key_file_load_from_file(keyfile, file, 0, &err)) {
		ofono_error("Parsing %s failed: %s", file, err->message);
		g_error_free(err);
		g_key_file_free(keyfile);
		return;
	}

	modems = g_key_file_get_groups(keyfile, NULL);

	for (i = 0; modems[i]; i++) {
		struct generic_at_data *parsed;
		struct ofono_modem *modem;

		DBG("modem: %s", modems[i]);

		parsed = parse_modem(keyfile, modems[i]);

		DBG("parsed: %p", parsed);

		if (parsed == NULL)
			continue;

		modem = ofono_modem_create(modems[i], "generic_at");

		if (modem == NULL)
			continue;

		g_modems = g_slist_prepend(g_modems, modem);

		ofono_modem_set_data(modem, parsed);
		ofono_modem_register(modem);
	}

	g_strfreev(modems);
	g_key_file_free(keyfile);
}

static void connect_destroy(gpointer user)
{
	struct ofono_modem *modem = user;
	struct generic_at_data *d = ofono_modem_get_data(modem);

	if (d->timeout_watcher != 0) {
		g_source_remove(d->timeout_watcher);
		d->timeout_watcher = 0;
	}

	d->io = NULL;
}

static void at_debug(const char *str, void *data)
{
	DBG("%s", str);
}

static gboolean connect_cb(GIOChannel *io, GIOCondition cond, gpointer user)
{
	struct ofono_modem *modem = user;
	struct generic_at_data *d = ofono_modem_get_data(modem);
	int err = 0;
	gboolean success;
	GAtSyntax *syntax;

	if (cond & G_IO_NVAL)
		return FALSE;

	if (cond & G_IO_OUT) {
		int sock = g_io_channel_unix_get_fd(io);
		socklen_t len = sizeof(err);

		if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
			err = errno == ENOTSOCK ? 0 : errno;
	} else if (cond & (G_IO_HUP | G_IO_ERR))
		err = ECONNRESET;

	success = !err;

	DBG("io ref: %d", io->ref_count);

	if (success == FALSE)
		goto error;

	syntax = g_at_syntax_new_gsmv1();
	d->chat = g_at_chat_new(io, syntax);
	g_at_syntax_unref(syntax);

	DBG("io ref: %d", io->ref_count);

	if (!d->chat)
		goto error;

	if (getenv("OFONO_AT_DEBUG") != NULL)
		g_at_chat_set_debug(d->chat, at_debug, NULL);

	if (d->timeout_command && d->timeout_interval > 0)
		g_at_chat_set_wakeup_command(d->chat, d->timeout_command, 1000,
					d->timeout_interval * 1000);

	DBG("%s", d->init_string);

	if (d->init_string)
		g_at_chat_send(d->chat, d->init_string, NULL, NULL, NULL, NULL);

	ofono_modem_set_powered(modem, TRUE);

	return FALSE;

error:
	ofono_modem_set_powered(modem, FALSE);
	return FALSE;
}

static gboolean connect_timeout(gpointer user)
{
	struct ofono_modem *modem = user;
	struct generic_at_data *d = ofono_modem_get_data(modem);

	d->timeout_watcher = 0;
	g_io_channel_close(d->io);
	ofono_modem_set_powered(modem, FALSE);

	return FALSE;
}

static GIOChannel *tty_connect(const char *tty)
{
	GIOChannel *io;
	int sk;
	struct termios newtio;

	sk = open(tty, O_RDWR | O_NOCTTY);

	if (sk < 0) {
		ofono_error("Can't open TTY %s: %s(%d)",
				tty, strerror(errno), errno);
		return NULL;
	}

	newtio.c_cflag = B115200 | CRTSCTS | CLOCAL | CREAD;
	newtio.c_iflag = IGNPAR;
	newtio.c_oflag = 0;
	newtio.c_lflag = 0;

	newtio.c_cc[VTIME] = 1;
	newtio.c_cc[VMIN] = 5;

	tcflush(sk, TCIFLUSH);
	if (tcsetattr(sk, TCSANOW, &newtio) < 0) {
		ofono_error("Can't change serial settings: %s(%d)",
				strerror(errno), errno);
		close(sk);
		return NULL;
	}

	io = g_io_channel_unix_new(sk);
	g_io_channel_set_close_on_unref(io, TRUE);

	if (g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK,
					NULL) != G_IO_STATUS_NORMAL) {
		g_io_channel_unref(io);
		return NULL;
	}

	return io;
}

static GIOChannel *socket_common(int sk, struct sockaddr *addr,
					socklen_t addrlen)
{
	GIOChannel *io = g_io_channel_unix_new(sk);

	if (io == NULL) {
		close(sk);
		return NULL;
	}

	g_io_channel_set_close_on_unref(io, TRUE);

	if (g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK,
					NULL) != G_IO_STATUS_NORMAL) {
		g_io_channel_unref(io);
		return NULL;
	}

	if (connect(sk, addr, addrlen) < 0) {
		if (errno != EAGAIN && errno != EINPROGRESS) {
			g_io_channel_unref(io);
			return NULL;
		}
	}

	return io;
}

static int generic_at_probe(struct ofono_modem *modem)
{
	return 0;
}

static void generic_at_remove(struct ofono_modem *modem)
{
	struct generic_at_data *d = ofono_modem_get_data(modem);

	generic_at_data_free(d);
}

static int generic_at_enable(struct ofono_modem *modem)
{
	struct generic_at_data *d = ofono_modem_get_data(modem);
	int sk;
	GIOChannel *io;
	GIOCondition cond;

	DBG("");

	switch (d->type) {
	case TRANSPORT_TYPE_TCP:
		sk = socket(PF_INET, SOCK_STREAM, 0);

		if (sk < 0)
			return -EAFNOSUPPORT;

		io = socket_common(sk, (struct sockaddr *)&d->tcp,
					sizeof(d->tcp));
		break;

	case TRANSPORT_TYPE_UNIX:
		sk = socket(AF_UNIX, SOCK_STREAM, 0);

		if (sk < 0)
			return -EAFNOSUPPORT;

		io = socket_common(sk, (struct sockaddr *)&d->tcp,
					sizeof(d->tcp));

		break;

	case TRANSPORT_TYPE_TTY:
		io = tty_connect(d->tty);

		break;

	default:
		io = NULL;
	}

	if (io == NULL)
		return -EINVAL;

	DBG("io ref: %d", io->ref_count);

	d->timeout_watcher = g_timeout_add_seconds(10, connect_timeout, modem);

	cond = G_IO_OUT | G_IO_ERR | G_IO_HUP | G_IO_NVAL;
	g_io_add_watch_full(io, G_PRIORITY_DEFAULT, cond, connect_cb,
				modem, connect_destroy);

	DBG("io ref: %d", io->ref_count);

	g_io_channel_unref(io);

	DBG("io ref: %d", io->ref_count);
	d->io = io;

	return -EINPROGRESS;
}

static int generic_at_disable(struct ofono_modem *modem)
{
	struct generic_at_data *d = ofono_modem_get_data(modem);

	if (d->io) {
		g_io_channel_close(d->io);
		d->io = NULL;
	}

	if (d->chat) {
		g_at_chat_unref(d->chat);
		d->chat = NULL;
	}

	return 0;
}

static void generic_at_populate(struct ofono_modem *modem)
{
	struct generic_at_data *d = ofono_modem_get_data(modem);
	GAtChat *chat = d->chat;
	struct ofono_message_waiting *mw;

	ofono_devinfo_create(modem, 0, "atmodem", chat);
	ofono_ussd_create(modem, 0, "atmodem", chat);
	ofono_sim_create(modem, 0, "atmodem", chat);
	ofono_call_forwarding_create(modem, 0, "atmodem", chat);
	ofono_call_settings_create(modem, 0, "atmodem", chat);
	ofono_netreg_create(modem, 0, "atmodem", chat);
	ofono_voicecall_create(modem, 0, "atmodem", chat);
	ofono_call_meter_create(modem, 0, "atmodem", chat);
	ofono_call_barring_create(modem, 0, "atmodem", chat);
	ofono_ssn_create(modem, 0, "atmodem", chat);
	ofono_sms_create(modem, 0, "atmodem", chat);
	ofono_phonebook_create(modem, 0, "atmodem", chat);

	mw = ofono_message_waiting_create(modem);
	if (mw)
		ofono_message_waiting_register(mw);
}

static struct ofono_modem_driver driver = {
	.name = "generic_at",
	.probe = generic_at_probe,
	.remove = generic_at_remove,
	.enable = generic_at_enable,
	.disable = generic_at_disable,
	.populate = generic_at_populate,
};

static int generic_at_init(void)
{
	int err = -EIO;

	err = ofono_modem_driver_register(&driver);

	if (err < 0)
		return err;

	parse_config(CONFIGDIR "/generic_at.conf");

	return 0;
}

static void generic_at_exit(void)
{
	GSList *l;
	struct ofono_modem *modem;

	for (l = g_modems; l; l = l->next) {
		modem = l->data;

		ofono_modem_remove(modem);
	}

	g_slist_free(g_modems);

	ofono_modem_driver_unregister(&driver);
}

OFONO_PLUGIN_DEFINE(generic_at, "Generic AT Modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			generic_at_init, generic_at_exit)
