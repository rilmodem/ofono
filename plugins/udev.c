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

#include <errno.h>
#include <ctype.h>
#include <stdlib.h>

#include <libudev.h>

#include <glib.h>
#include <string.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

static GSList *modem_list = NULL;
static GHashTable *devpath_list = NULL;

static struct ofono_modem *find_modem(const char *devpath)
{
	GSList *list;

	for (list = modem_list; list; list = list->next) {
		struct ofono_modem *modem = list->data;
		const char *path = ofono_modem_get_string(modem, "Path");

		if (g_strcmp0(devpath, path) == 0)
			return modem;
	}

	return NULL;
}

static const char *get_property(struct udev_device *device,
				char const *property_name)
{
	struct udev_list_entry *entry;

	entry = udev_device_get_properties_list_entry(device);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);

		if (g_strcmp0(name, property_name) == 0)
			return udev_list_entry_get_value(entry);

		entry = udev_list_entry_get_next(entry);
	}

	return NULL;
}

static const char *get_driver(struct udev_device *udev_device)
{
	return get_property(udev_device, "OFONO_DRIVER");
}

static const char *get_serial(struct udev_device *udev_device)
{
	const char *serial;

	serial = get_property(udev_device, "ID_SERIAL_SHORT");

	if (serial != NULL) {
		unsigned int i, len = strlen(serial);

		for (i = 0; i < len; i++) {
			if (!g_ascii_isalnum(serial[i]))
				return NULL;
		}
	}

	return serial;
}

static void add_ifx(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	struct udev_list_entry *entry;
	const char *devnode;

	DBG("modem %p", modem);

	devnode = udev_device_get_devnode(udev_device);
	ofono_modem_set_string(modem, "Device", devnode);

	entry = udev_device_get_properties_list_entry(udev_device);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		const char *value = udev_list_entry_get_value(entry);

		if (g_str_equal(name, "OFONO_IFX_LDISC") == TRUE)
			ofono_modem_set_string(modem, "LineDiscipline", value);
		else if (g_str_equal(name, "OFONO_IFX_AUDIO") == TRUE)
			ofono_modem_set_string(modem, "AudioSetting", value);
		else if (g_str_equal(name, "OFONO_IFX_LOOPBACK") == TRUE)
			ofono_modem_set_string(modem, "AudioLoopback", value);

		entry = udev_list_entry_get_next(entry);
	}

	ofono_modem_register(modem);
}

static void add_isi(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *ifname, *type, *addr;

	DBG("modem %p", modem);

	if (ofono_modem_get_string(modem, "Interface"))
		return;

	addr = get_property(udev_device, "OFONO_ISI_ADDRESS");
	if (addr != NULL)
		ofono_modem_set_integer(modem, "Address", atoi(addr));

	if (g_strcmp0(udev_device_get_subsystem(udev_device), "net") != 0)
		return;

	type = udev_device_get_sysattr_value(udev_device, "type");
	if (g_strcmp0(type, "820") != 0)
		return;

	ifname = udev_device_get_sysname(udev_device);
	ofono_modem_set_string(modem, "Interface", ifname);

	DBG("interface %s", ifname);

	ofono_modem_register(modem);
}

static void add_calypso(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *devnode;

	DBG("modem %p", modem);

	devnode = udev_device_get_devnode(udev_device);
	ofono_modem_set_string(modem, "Device", devnode);

	ofono_modem_register(modem);
}

static void add_tc65(struct ofono_modem *modem,
			struct udev_device *udev_device)
{
	const char *devnode;

	DBG("modem %p", modem);

	devnode = udev_device_get_devnode(udev_device);
	ofono_modem_set_string(modem, "Device", devnode);

	ofono_modem_register(modem);
}

static void add_nokiacdma(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *devnode;

	DBG("modem %p", modem);

	devnode = udev_device_get_devnode(udev_device);
	ofono_modem_set_string(modem, "Device", devnode);

	ofono_modem_register(modem);
}

static void add_sim900(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *devnode;

	DBG("modem %p", modem);

	devnode = udev_device_get_devnode(udev_device);
	ofono_modem_set_string(modem, "Device", devnode);

	ofono_modem_register(modem);
}

static void add_modem(struct udev_device *udev_device)
{
	struct ofono_modem *modem;
	struct udev_device *parent;
	const char *devpath, *curpath, *driver;

	driver = get_driver(udev_device);
	if (driver != NULL) {
		devpath = udev_device_get_devpath(udev_device);
		if (devpath == NULL)
			return;

		modem = ofono_modem_create(NULL, driver);
		if (modem == NULL)
			return;

		ofono_modem_set_string(modem, "Path", devpath);

		modem_list = g_slist_prepend(modem_list, modem);

		goto done;
	}

	parent = udev_device_get_parent(udev_device);
	if (parent == NULL)
		return;

	driver = get_driver(parent);
	if (driver == NULL) {
		parent = udev_device_get_parent(parent);
		driver = get_driver(parent);
		if (driver == NULL) {
			parent = udev_device_get_parent(parent);
			driver = get_driver(parent);
			if (driver == NULL)
				return;
		}
	}

	devpath = udev_device_get_devpath(parent);
	if (devpath == NULL)
		return;

	modem = find_modem(devpath);
	if (modem == NULL) {
		const char *serial = get_serial(parent);

		modem = ofono_modem_create(serial, driver);
		if (modem == NULL)
			return;

		ofono_modem_set_string(modem, "Path", devpath);
		ofono_modem_set_integer(modem, "Registered", 0);

		modem_list = g_slist_prepend(modem_list, modem);
	}

done:
	curpath = udev_device_get_devpath(udev_device);
	if (curpath == NULL)
		return;

	DBG("%s (%s)", curpath, driver);

	g_hash_table_insert(devpath_list, g_strdup(curpath), g_strdup(devpath));

	if (g_strcmp0(driver, "ifx") == 0)
		add_ifx(modem, udev_device);
	else if (g_strcmp0(driver, "u8500") == 0)
		add_isi(modem, udev_device);
	else if (g_strcmp0(driver, "n900") == 0)
		add_isi(modem, udev_device);
	else if (g_strcmp0(driver, "calypso") == 0)
		add_calypso(modem, udev_device);
	else if (g_strcmp0(driver, "tc65") == 0)
		add_tc65(modem, udev_device);
	else if (g_strcmp0(driver, "nokiacdma") == 0)
		add_nokiacdma(modem, udev_device);
	else if (g_strcmp0(driver, "sim900") == 0)
		add_sim900(modem, udev_device);
}

static gboolean devpath_remove(gpointer key, gpointer value, gpointer user_data)
{
	const char *path = value;
	const char *devpath = user_data;

	DBG("%s -> %s", path, devpath);

	return g_str_equal(path, devpath);
}

static void remove_modem(struct udev_device *udev_device)
{
	struct ofono_modem *modem;
	const char *curpath = udev_device_get_devpath(udev_device);
	char *devpath, *remove;

	if (curpath == NULL)
		return;

	DBG("%s", curpath);

	devpath = g_hash_table_lookup(devpath_list, curpath);
	if (devpath == NULL)
		return;

	modem = find_modem(devpath);
	if (modem == NULL)
		return;

	modem_list = g_slist_remove(modem_list, modem);

	ofono_modem_remove(modem);

	DBG("%s", devpath);

	remove = g_strdup(devpath);

	g_hash_table_foreach_remove(devpath_list, devpath_remove, remove);

	g_free(remove);
}

static void enumerate_devices(struct udev *context)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *entry;

	enumerate = udev_enumerate_new(context);
	if (enumerate == NULL)
		return;

	udev_enumerate_add_match_subsystem(enumerate, "tty");
	udev_enumerate_add_match_subsystem(enumerate, "net");
	udev_enumerate_add_match_subsystem(enumerate, "hsi");

	udev_enumerate_scan_devices(enumerate);

	entry = udev_enumerate_get_list_entry(enumerate);
	while (entry) {
		const char *syspath = udev_list_entry_get_name(entry);
		struct udev_device *device;

		device = udev_device_new_from_syspath(context, syspath);
		if (device != NULL) {
			const char *subsystem;

			subsystem = udev_device_get_subsystem(device);

			if (g_strcmp0(subsystem, "tty") == 0 ||
					g_strcmp0(subsystem, "net") == 0 ||
					g_strcmp0(subsystem, "hsi") == 0)
				add_modem(device);

			udev_device_unref(device);
		}

		entry = udev_list_entry_get_next(entry);
	}

	udev_enumerate_unref(enumerate);
}

static struct udev *udev_ctx;
static struct udev_monitor *udev_mon;
static guint udev_watch = 0;

static gboolean udev_event(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct udev_device *device;
	const char *subsystem, *action;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		ofono_warn("Error with udev monitor channel");
		udev_watch = 0;
		return FALSE;
	}

	device = udev_monitor_receive_device(udev_mon);
	if (device == NULL)
		return TRUE;

	subsystem = udev_device_get_subsystem(device);
	if (subsystem == NULL)
		goto done;

	action = udev_device_get_action(device);
	if (action == NULL)
		goto done;

	DBG("subsystem %s %s", subsystem, action);

	if (g_str_equal(action, "add") == TRUE) {
		if (g_strcmp0(subsystem, "tty") == 0 ||
				g_strcmp0(subsystem, "net") == 0 ||
					g_strcmp0(subsystem, "hsi") == 0)
			add_modem(device);
	} else if (g_str_equal(action, "remove") == TRUE) {
		if (g_strcmp0(subsystem, "tty") == 0 ||
				g_strcmp0(subsystem, "net") == 0 ||
					g_strcmp0(subsystem, "hsi") == 0)
			remove_modem(device);
	}

	DBG("subsystem %s finished", subsystem);

done:
	udev_device_unref(device);

	return TRUE;
}

static void udev_start(void)
{
	GIOChannel *channel;
	int fd;

	if (udev_monitor_enable_receiving(udev_mon) < 0) {
		ofono_error("Failed to enable udev monitor");
		return;
	}

	enumerate_devices(udev_ctx);

	fd = udev_monitor_get_fd(udev_mon);

	channel = g_io_channel_unix_new(fd);
	if (channel == NULL)
		return;

	udev_watch = g_io_add_watch(channel,
				G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL,
							udev_event, NULL);

	g_io_channel_unref(channel);
}

static int udev_init(void)
{
	devpath_list = g_hash_table_new_full(g_str_hash, g_str_equal,
						g_free, g_free);
	if (devpath_list == NULL) {
		ofono_error("Failed to create udev path list");
		return -ENOMEM;
	}

	udev_ctx = udev_new();
	if (udev_ctx == NULL) {
		ofono_error("Failed to create udev context");
		g_hash_table_destroy(devpath_list);
		return -EIO;
	}

	udev_mon = udev_monitor_new_from_netlink(udev_ctx, "udev");
	if (udev_mon == NULL) {
		ofono_error("Failed to create udev monitor");
		g_hash_table_destroy(devpath_list);
		udev_unref(udev_ctx);
		udev_ctx = NULL;
		return -EIO;
	}

	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "tty", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "net", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "hsi", NULL);

	udev_monitor_filter_update(udev_mon);

	udev_start();

	return 0;
}

static void udev_exit(void)
{
	GSList *list;

	if (udev_watch > 0)
		g_source_remove(udev_watch);

	for (list = modem_list; list; list = list->next) {
		struct ofono_modem *modem = list->data;

		ofono_modem_remove(modem);
	}

	g_slist_free(modem_list);
	modem_list = NULL;

	g_hash_table_destroy(devpath_list);
	devpath_list = NULL;

	if (udev_ctx == NULL)
		return;

	udev_monitor_filter_remove(udev_mon);

	udev_monitor_unref(udev_mon);
	udev_unref(udev_ctx);
}

OFONO_PLUGIN_DEFINE(udev, "udev hardware detection", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, udev_init, udev_exit)
