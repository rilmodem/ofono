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
			if (!isalnum(serial[i]))
				return NULL;
		}
	}

	return serial;
}

#define MODEM_DEVICE		"ModemDevice"
#define DATA_DEVICE		"DataDevice"
#define GPS_DEVICE		"GPSDevice"
#define NETWORK_INTERFACE	"NetworkInterface"

static void add_mbm(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *desc, *devnode;
	const char *device, *data, *network;
	int registered;

	desc = udev_device_get_sysattr_value(udev_device, "device/interface");

	if (desc == NULL)
		return;

	DBG("desc: %s", desc);

	registered = ofono_modem_get_integer(modem, "Registered");

	if (registered == 0 &&
			(g_str_has_suffix(desc, "Minicard Modem") ||
			g_str_has_suffix(desc, "Minicard Modem 2") ||
			g_str_has_suffix(desc, "Mini-Card Modem") ||
			g_str_has_suffix(desc, "Broadband Modem") ||
			g_str_has_suffix(desc, "Module Modem") ||
			g_str_has_suffix(desc, "Broadband USB Modem"))) {
		devnode = udev_device_get_devnode(udev_device);

		if (ofono_modem_get_string(modem, MODEM_DEVICE) == NULL)
			ofono_modem_set_string(modem, MODEM_DEVICE, devnode);
		else
			ofono_modem_set_string(modem, DATA_DEVICE, devnode);
	} else if (registered == 0 &&
			(g_str_has_suffix(desc, "Minicard Data Modem") ||
			g_str_has_suffix(desc, "Mini-Card Data Modem") ||
			g_str_has_suffix(desc, "Module Data Modem") ||
			g_str_has_suffix(desc, "Module\xc2\xa0""Data Modem") ||
			g_str_has_suffix(desc, "Broadband Data Modem"))) {
		devnode = udev_device_get_devnode(udev_device);
		ofono_modem_set_string(modem, DATA_DEVICE, devnode);
	} else if (g_str_has_suffix(desc, "Minicard GPS Port") ||
			g_str_has_suffix(desc, "Mini-Card GPS Port") ||
			g_str_has_suffix(desc, "Module NMEA") ||
			g_str_has_suffix(desc, "Broadband GPS Port")) {
		devnode = udev_device_get_devnode(udev_device);
		ofono_modem_set_string(modem, GPS_DEVICE, devnode);
	} else if (registered == 0 &&
			(g_str_has_suffix(desc, "Minicard Network Adapter") ||
			g_str_has_suffix(desc, "Mini-Card Network Adapter") ||
			g_str_has_suffix(desc, "Broadband Network Adapter") ||
			g_str_has_suffix(desc, "Module Network Adapter") ||
			g_str_has_suffix(desc, "Minicard NetworkAdapter"))) {
		devnode = get_property(udev_device, "INTERFACE");
		ofono_modem_set_string(modem, NETWORK_INTERFACE, devnode);
	} else {
		return;
	}

	if (registered == 1)
		return;

	device  = ofono_modem_get_string(modem, MODEM_DEVICE);
	data = ofono_modem_get_string(modem, DATA_DEVICE);
	network = ofono_modem_get_string(modem, NETWORK_INTERFACE);

	if (device != NULL && data != NULL && network != NULL) {
		ofono_modem_set_integer(modem, "Registered", 1);
		ofono_modem_register(modem);
	}
}

#define APPLICATION_PORT "ApplicationPort"
#define CONTROL_PORT "ControlPort"

static void add_hso(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *subsystem, *type, *devnode;
	const char *app, *control, *network;
	int registered;

	DBG("modem %p", modem);

	subsystem = udev_device_get_subsystem(udev_device);
	if (subsystem == NULL)
		return;

	registered = ofono_modem_get_integer(modem, "Registered");
	if (registered != 0)
		return;

	type = udev_device_get_sysattr_value(udev_device, "hsotype");

	if (type != NULL) {
		devnode = udev_device_get_devnode(udev_device);

		if (g_str_has_suffix(type, "Application") == TRUE)
			ofono_modem_set_string(modem, APPLICATION_PORT, devnode);
		else if (g_str_has_suffix(type, "Control") == TRUE)
			ofono_modem_set_string(modem, CONTROL_PORT, devnode);
	} else if (g_str_equal(subsystem, "net") == TRUE) {
		devnode = get_property(udev_device, "INTERFACE");
		ofono_modem_set_string(modem, NETWORK_INTERFACE, devnode);
	} else {
		return;
	}

	app = ofono_modem_get_string(modem, APPLICATION_PORT);
	control = ofono_modem_get_string(modem, CONTROL_PORT);
	network = ofono_modem_get_string(modem, NETWORK_INTERFACE);

	if (app != NULL && control != NULL && network != NULL) {
		ofono_modem_set_integer(modem, "Registered", 1);
		ofono_modem_register(modem);
	}
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

static void add_zte(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	struct udev_list_entry *entry;
	const char *devnode, *type;

	int ppp, aux;

	DBG("modem %p", modem);

	ppp = ofono_modem_get_integer(modem, "ModemRegistered");
	aux = ofono_modem_get_integer(modem, "AuxRegistered");

	if (ppp && aux)
		return;

	entry = udev_device_get_properties_list_entry(udev_device);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		type = udev_list_entry_get_value(entry);

		if (g_str_equal(name, "OFONO_ZTE_TYPE") != TRUE) {
			entry = udev_list_entry_get_next(entry);
			continue;
		}

		if (g_str_equal(type, "modem") == TRUE) {
			if (ppp != 0)
				return;

			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "Modem", devnode);
			ppp = 1;
			ofono_modem_set_integer(modem, "ModemRegistered", ppp);
		} else if (g_str_equal(type, "aux") == TRUE) {
			if (aux != 0)
				return;

			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "Aux", devnode);

			aux = 1;
			ofono_modem_set_integer(modem, "AuxRegistered", aux);
		}

		break;
	}

	if (ppp && aux)
		ofono_modem_register(modem);
}

static void add_huawei(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	struct udev_list_entry *entry;
	const char *devnode;
	int ppp, pcui;

	DBG("modem %p", modem);

	/*
	 * Huawei dongles tend to break up their ports into:
	 * - Modem - Used for PPP
	 * - Diag - Used for diagnostics, not usually AT command enabled
	 * - PCUI - auxiliary channel where unsolicited events are sent
	 *
	 * The unsolicited events are controlled with ^PORTSEL command,
	 * and defaults to 0 (the PCUI port)
	 *
	 * Surprising the PCUI port is usually last on the usb interface list
	 */
	ppp = ofono_modem_get_integer(modem, "ModemRegistered");
	pcui = ofono_modem_get_integer(modem, "PcuiRegistered");

	if (ppp && pcui)
		return;

	entry = udev_device_get_properties_list_entry(udev_device);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		const char *type = udev_list_entry_get_value(entry);

		if (g_str_equal(name, "OFONO_HUAWEI_TYPE") != TRUE) {
			entry = udev_list_entry_get_next(entry);
			continue;
		}

		if (g_str_equal(type, "Modem") == TRUE) {
			if (ppp != 0)
				return;

			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "Modem", devnode);
			ppp = 1;
			ofono_modem_set_integer(modem, "ModemRegistered", ppp);
		} else if (g_str_equal(type, "Pcui") == TRUE) {
			if (pcui != 0)
				return;

			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "Pcui", devnode);

			pcui = 1;
			ofono_modem_set_integer(modem, "PcuiRegistered", pcui);
		}

		break;
	}

	if (ppp && pcui)
		ofono_modem_register(modem);
}

static void add_sierra(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	struct udev_list_entry *entry;
	const char *devnode;
	gboolean found = FALSE;

	DBG("modem %p", modem);

	entry = udev_device_get_properties_list_entry(udev_device);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		const char *value = udev_list_entry_get_value(entry);

		if (g_str_equal(name, "OFONO_SIERRA_TYPE") == TRUE &&
					g_str_equal(value, "modem") == TRUE) {
			found = TRUE;
			break;
		}

		entry = udev_list_entry_get_next(entry);
	}

	if (found == FALSE)
		return;

	devnode = udev_device_get_devnode(udev_device);
	ofono_modem_set_string(modem, "Device", devnode);

	ofono_modem_register(modem);
}

static void add_novatel(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *devnode, *intfnum;
	int registered;

	DBG("modem %p", modem);

	registered = ofono_modem_get_integer(modem, "Registered");

        if (registered > 1)
                return;

	intfnum = get_property(udev_device, "ID_USB_INTERFACE_NUM");

	DBG("intfnum %s", intfnum);

	if (g_strcmp0(intfnum, "00") == 0) {
		devnode = udev_device_get_devnode(udev_device);
		ofono_modem_set_string(modem, "PrimaryDevice", devnode);

		ofono_modem_set_integer(modem, "Registered", ++registered);
	} else if (g_strcmp0(intfnum, "01") == 0) {
		devnode = udev_device_get_devnode(udev_device);
		ofono_modem_set_string(modem, "SecondaryDevice", devnode);

		ofono_modem_set_integer(modem, "Registered", ++registered);
	}

	if (registered > 1)
		ofono_modem_register(modem);
}

static void add_nokia(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *devnode, *intfnum;
	int registered;

	DBG("modem %p", modem);

	registered = ofono_modem_get_integer(modem, "Registered");
	if (registered != 0)
		return;

	intfnum = get_property(udev_device, "ID_USB_INTERFACE_NUM");

	DBG("intfnum %s", intfnum);

	if (g_strcmp0(intfnum, "02") == 0) {
		devnode = udev_device_get_devnode(udev_device);
		ofono_modem_set_string(modem, "Modem", devnode);
	} else if (g_strcmp0(intfnum, "04") == 0) {
		devnode = udev_device_get_devnode(udev_device);
		ofono_modem_set_string(modem, "Control", devnode);

		ofono_modem_set_integer(modem, "Registered", 1);
		ofono_modem_register(modem);
	}
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

static void add_gobi(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	struct udev_list_entry *entry;
	const char *devnode;
	gboolean found = FALSE;

	DBG("modem %p", modem);

	entry = udev_device_get_properties_list_entry(udev_device);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		const char *value = udev_list_entry_get_value(entry);

		if (g_str_equal(name, "OFONO_GOBI_TYPE") == TRUE &&
					g_str_equal(value, "modem") == TRUE) {
			found = TRUE;
			break;
		}

		entry = udev_list_entry_get_next(entry);
	}

	if (found == FALSE)
		return;

	devnode = udev_device_get_devnode(udev_device);
	ofono_modem_set_string(modem, "Device", devnode);

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

static void add_linktop(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *devnode, *intfnum;
	int registered;

	DBG("modem %p", modem);

	registered = ofono_modem_get_integer(modem, "Registered");
	if (registered != 0)
		return;

	intfnum = get_property(udev_device, "ID_USB_INTERFACE_NUM");

	DBG("intfnum %s", intfnum);

	if (g_strcmp0(intfnum, "01") == 0) {
		devnode = udev_device_get_devnode(udev_device);
		ofono_modem_set_string(modem, "Aux", devnode);
	} else if (g_strcmp0(intfnum, "03") == 0) {
		devnode = udev_device_get_devnode(udev_device);
		ofono_modem_set_string(modem, "Modem", devnode);

		ofono_modem_set_integer(modem, "Registered", 1);
		ofono_modem_register(modem);
	}
}

static void add_telit(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	struct udev_list_entry *entry;
	const char *devnode, *type;
	int registered;

	DBG("modem %p", modem);

	registered = ofono_modem_get_integer(modem, "Registered");

	entry = udev_device_get_properties_list_entry(udev_device);

	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		type = udev_list_entry_get_value(entry);

		if (g_str_equal(name, "OFONO_TELIT_TYPE") != TRUE) {
			entry = udev_list_entry_get_next(entry);
			continue;
		}

		if (registered == 0 && g_str_equal(type, "Modem") == TRUE) {
			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "Modem", devnode);
		} else if (g_str_equal(type, "GPS") == TRUE) {
			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "GPS", devnode);
		} else if (registered == 0 &&
				g_str_equal(type, "Data") == TRUE) {
			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "Data", devnode);
		}

		break;
	}

	if (registered == 1)
		return;

	if (ofono_modem_get_string(modem, "Modem") != NULL &&
			ofono_modem_get_string(modem, "Data") != NULL) {
		ofono_modem_set_integer(modem, "Registered", 1);
		ofono_modem_register(modem);
	}
}

static void add_speedup(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	struct udev_list_entry *entry;
	const char *devnode, *type;

	int ppp, aux;

	DBG("modem %p", modem);

	ppp = ofono_modem_get_integer(modem, "ModemRegistered");
	aux = ofono_modem_get_integer(modem, "AuxRegistered");

	if (ppp && aux)
		return;

	entry = udev_device_get_properties_list_entry(udev_device);
	while (entry) {
		const char *name = udev_list_entry_get_name(entry);
		type = udev_list_entry_get_value(entry);

		if (g_str_equal(name, "OFONO_SPEEDUP_TYPE") != TRUE) {
			entry = udev_list_entry_get_next(entry);
			continue;
		}

		if (g_str_equal(type, "modem") == TRUE) {
			if (ppp != 0)
				return;

			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "Modem", devnode);
			ppp = 1;
			ofono_modem_set_integer(modem, "ModemRegistered", ppp);
		} else if (g_str_equal(type, "aux") == TRUE) {
			if (aux != 0)
				return;

			devnode = udev_device_get_devnode(udev_device);
			ofono_modem_set_string(modem, "Aux", devnode);

			aux = 1;
			ofono_modem_set_integer(modem, "AuxRegistered", aux);
		}

		break;
	}

	if (ppp && aux)
		ofono_modem_register(modem);
}

static void add_samsung(struct ofono_modem *modem,
					struct udev_device *udev_device)
{
	const char *subsystem;

	DBG("modem %p", modem);

	subsystem = udev_device_get_subsystem(udev_device);
	if (subsystem == NULL)
		return;

	if (g_str_equal(subsystem, "net") == TRUE) {
		const char *interface;

		interface = get_property(udev_device, "INTERFACE");
		if (interface == NULL)
			return;

		DBG("network %s", interface);

		ofono_modem_set_string(modem, "Network", interface);
	} else if (g_str_equal(subsystem, "tty") == TRUE) {
		const char *devnode;

		devnode = udev_device_get_devnode(udev_device);
		if (devnode == NULL)
			return;

		DBG("device %s", devnode);

		ofono_modem_set_string(modem, "Device", devnode);
		ofono_modem_register(modem);
	}
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

	if (g_strcmp0(driver, "mbm") == 0)
		add_mbm(modem, udev_device);
	else if (g_strcmp0(driver, "hso") == 0)
		add_hso(modem, udev_device);
	else if (g_strcmp0(driver, "ifx") == 0)
		add_ifx(modem, udev_device);
	else if (g_strcmp0(driver, "zte") == 0)
		add_zte(modem, udev_device);
	else if (g_strcmp0(driver, "huawei") == 0)
		add_huawei(modem, udev_device);
	else if (g_strcmp0(driver, "huaweicdma") == 0)
		add_huawei(modem, udev_device);
	else if (g_strcmp0(driver, "sierra") == 0)
		add_sierra(modem, udev_device);
	else if (g_strcmp0(driver, "novatel") == 0)
		add_novatel(modem, udev_device);
	else if (g_strcmp0(driver, "nokia") == 0)
		add_nokia(modem, udev_device);
	else if (g_strcmp0(driver, "isiusb") == 0)
		add_isi(modem, udev_device);
	else if (g_strcmp0(driver, "u8500") == 0)
		add_isi(modem, udev_device);
	else if (g_strcmp0(driver, "n900") == 0)
		add_isi(modem, udev_device);
	else if (g_strcmp0(driver, "gobi") == 0)
		add_gobi(modem, udev_device);
	else if (g_strcmp0(driver, "calypso") == 0)
		add_calypso(modem, udev_device);
	else if (g_strcmp0(driver, "tc65") == 0)
		add_tc65(modem, udev_device);
	else if (g_strcmp0(driver, "telit") == 0)
		add_telit(modem, udev_device);
	else if (g_strcmp0(driver, "nokiacdma") == 0)
		add_nokiacdma(modem, udev_device);
	else if (g_strcmp0(driver, "linktop") == 0)
		add_linktop(modem, udev_device);
	else if (g_strcmp0(driver, "speedup") == 0)
		add_speedup(modem, udev_device);
	else if (g_strcmp0(driver, "speedupcdma") == 0)
		add_speedup(modem, udev_device);
	else if (g_strcmp0(driver, "samsung") == 0)
		add_samsung(modem, udev_device);
}

static gboolean devpath_remove(gpointer key, gpointer value, gpointer user_data)
{
	const char *path = value;
	const char *devpath = user_data;

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

static gboolean udev_event(GIOChannel *channel,
				GIOCondition condition, gpointer user_data)
{
	struct udev_monitor *monitor = user_data;
	struct udev_device *device;
	const char *subsystem, *action;

	device = udev_monitor_receive_device(monitor);
	if (device == NULL)
		return TRUE;

	subsystem = udev_device_get_subsystem(device);
	if (subsystem == NULL)
		goto done;

	action = udev_device_get_action(device);
	if (action == NULL)
		goto done;

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

done:
	udev_device_unref(device);

	return TRUE;
}

static struct udev *udev_ctx;
static struct udev_monitor *udev_mon;
static guint udev_watch = 0;

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

	udev_watch = g_io_add_watch(channel, G_IO_IN, udev_event, udev_mon);

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
