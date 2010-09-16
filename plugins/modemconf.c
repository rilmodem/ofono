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
#include <stdlib.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

static GSList *modem_list = NULL;

static const char *tty_opts[] = {
	"Baud",
	"Read",
	"Local",
	"StopBits",
	"DataBits",
	"Parity",
	"XonXoff",
	"RtsCts",
	"GsmSyntax",
	NULL,
};

static int set_address(struct ofono_modem *modem,
					GKeyFile *keyfile, const char *group)
{
	char *value;

	value = g_key_file_get_string(keyfile, group, "Address", NULL);
	if (value) {
		ofono_modem_set_string(modem, "Address", value);
		g_free(value);
	} else {
		ofono_modem_set_string(modem, "Address", "127.0.0.1");
	}

	value = g_key_file_get_string(keyfile, group, "Port", NULL);
	if (value) {
		ofono_modem_set_integer(modem, "Port", atoi(value));
		g_free(value);
	} else {
		ofono_modem_set_integer(modem, "Port", 12345);
	}

	value = g_key_file_get_string(keyfile, group, "Modem", NULL);
	if (value) {
		ofono_modem_set_string(modem, "Modem", value);
		g_free(value);
	}

	value = g_key_file_get_string(keyfile, group, "Multiplexer", NULL);
	if (value) {
		ofono_modem_set_string(modem, "Multiplexer", value);
		g_free(value);
	}

	return 0;
}

static int set_device(struct ofono_modem *modem,
					GKeyFile *keyfile, const char *group)
{
	char *device;
	char *value;
	int i;

	device = g_key_file_get_string(keyfile, group, "Device", NULL);
	if (!device)
		return -EINVAL;

	ofono_modem_set_string(modem, "Device", device);

	g_free(device);

	for (i = 0; tty_opts[i]; i++) {
		value = g_key_file_get_string(keyfile, group,
						tty_opts[i], NULL);

		if (value == NULL)
			continue;

		ofono_modem_set_string(modem, tty_opts[i], value);
		g_free(value);
	}

	return 0;
}

static int set_interface(struct ofono_modem *modem,
					GKeyFile *keyfile, const char *group)
{
	char *value;

	value = g_key_file_get_string(keyfile, group, "Interface", NULL);
	if (value)
		ofono_modem_set_string(modem, "Interface", value);
	g_free(value);

	value = g_key_file_get_string(keyfile, group, "Address", NULL);
	if (value)
		ofono_modem_set_integer(modem, "Address", atoi(value));
	g_free(value);

	return 0;
}

static struct {
	const char *driver;
	int (*func) (struct ofono_modem *modem,
				GKeyFile *keyfile, const char *group);
} setup_helpers[] = {
	{ "phonesim",	set_address	},
	{ "atgen",	set_device	},
	{ "g1",		set_device	},
	{ "wavecom",	set_device	},
	{ "ste",	set_device	},
	{ "ste",	set_interface	},
	{ "calypso",	set_device	},
	{ "palmpre",	set_device	},
	{ "isigen",     set_interface   },
	{ "n900",	set_interface	},
	{ NULL }
};

static struct ofono_modem *create_modem(GKeyFile *keyfile, const char *group)
{
	struct ofono_modem *modem;
	char *driver;
	int i;

	driver = g_key_file_get_string(keyfile, group, "Driver", NULL);
	if (!driver)
		return NULL;

	modem = ofono_modem_create(group, driver);
	if (modem == NULL)
		goto error;

	for (i = 0; setup_helpers[i].driver; i++) {
		if (!g_strcmp0(driver, setup_helpers[i].driver))
			setup_helpers[i].func(modem, keyfile, group);
	}

error:
	g_free(driver);

	return modem;
}

static void parse_config(const char *file)
{
	GKeyFile *keyfile;
	GError *err = NULL;
	char **modems;
	int i;

	keyfile = g_key_file_new();

	g_key_file_set_list_separator(keyfile, ',');

	if (!g_key_file_load_from_file(keyfile, file, 0, &err)) {
		ofono_warn("Reading of %s failed: %s", file, err->message);
		g_error_free(err);
		goto done;
	}

	modems = g_key_file_get_groups(keyfile, NULL);

	for (i = 0; modems[i]; i++) {
		struct ofono_modem *modem;

		modem = create_modem(keyfile, modems[i]);
		if (!modem)
			continue;

		modem_list = g_slist_prepend(modem_list, modem);

		ofono_modem_register(modem);
	}

	g_strfreev(modems);

done:
	g_key_file_free(keyfile);
}

static int modemconf_init(void)
{
	parse_config(CONFIGDIR "/modem.conf");

	return 0;
}

static void modemconf_exit(void)
{
	GSList *list;

	for (list = modem_list; list; list = list->next) {
		struct ofono_modem *modem = list->data;

		ofono_modem_remove(modem);
	}

	g_slist_free(modem_list);
	modem_list = NULL;
}

OFONO_PLUGIN_DEFINE(modemconf, "Static modem configuration", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, modemconf_init, modemconf_exit)
