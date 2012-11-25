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
#include <string.h>

#include <libudev.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

struct modem_info {
	char *syspath;
	char *devname;
	char *driver;
	char *vendor;
	char *model;
	GSList *devices;
	struct ofono_modem *modem;
	const char *sysattr;
};

struct device_info {
	char *devpath;
	char *devnode;
	char *interface;
	char *number;
	char *label;
	char *sysattr;
};

static gboolean setup_isi(struct modem_info *modem)
{
	const char *node = NULL;
	int addr = 0;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->sysattr);

		if (g_strcmp0(info->sysattr, "820") == 0) {
			if (g_strcmp0(info->interface, "2/254/0") == 0)
				addr = 16;

			node = info->devnode;
		}
	}

	if (node == NULL)
		return FALSE;

	DBG("interface=%s address=%d", node, addr);

	ofono_modem_set_string(modem->modem, "Interface", node);
	ofono_modem_set_integer(modem->modem, "Address", addr);

	return TRUE;
}

static gboolean setup_mbm(struct modem_info *modem)
{
	const char *mdm = NULL, *app = NULL, *network = NULL, *gps = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->sysattr);

		if (g_str_has_suffix(info->sysattr, "Modem") == TRUE ||
				g_str_has_suffix(info->sysattr,
							"Modem 2") == TRUE) {
			if (mdm == NULL)
				mdm = info->devnode;
			else
				app = info->devnode;
		} else if (g_str_has_suffix(info->sysattr,
						"GPS Port") == TRUE ||
				g_str_has_suffix(info->sysattr,
						"Module NMEA") == TRUE) {
			gps = info->devnode;
		} else if (g_str_has_suffix(info->sysattr,
						"Network Adapter") == TRUE ||
				g_str_has_suffix(info->sysattr,
						"NetworkAdapter") == TRUE) {
			network = info->devnode;
		}
	}

	if (mdm == NULL || app == NULL)
		return FALSE;

	DBG("modem=%s data=%s network=%s gps=%s", mdm, app, network, gps);

	ofono_modem_set_string(modem->modem, "ModemDevice", mdm);
	ofono_modem_set_string(modem->modem, "DataDevice", app);
	ofono_modem_set_string(modem->modem, "GPSDevice", gps);
	ofono_modem_set_string(modem->modem, "NetworkInterface", network);

	return TRUE;
}

static gboolean setup_hso(struct modem_info *modem)
{
	const char *ctl = NULL, *app = NULL, *mdm = NULL, *net = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s %s", info->devnode, info->interface,
				info->number, info->label, info->sysattr);

		if (g_strcmp0(info->sysattr, "Control") == 0)
			ctl = info->devnode;
		else if (g_strcmp0(info->sysattr, "Application") == 0)
			app = info->devnode;
		else if (g_strcmp0(info->sysattr, "Modem") == 0)
			mdm = info->devnode;
		else if (info->sysattr == NULL &&
				g_str_has_prefix(info->devnode, "hso") == TRUE)
			net = info->devnode;
	}

	if (ctl == NULL || app == NULL)
		return FALSE;

	DBG("control=%s application=%s modem=%s network=%s",
						ctl, app, mdm, net);

	ofono_modem_set_string(modem->modem, "Control", ctl);
	ofono_modem_set_string(modem->modem, "Application", app);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_gobi(struct modem_info *modem)
{
	const char *qmi = NULL, *mdm = NULL, *net = NULL;
	const char *gps = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (info->number == NULL)
				qmi = info->devnode;
			else if (g_strcmp0(info->number, "00") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				diag = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				gps = info->devnode;
		}
	}

	if (qmi == NULL || mdm == NULL || net == NULL)
		return FALSE;

	DBG("qmi=%s net=%s mdm=%s gps=%s diag=%s", qmi, net, mdm, gps, diag);

	ofono_modem_set_string(modem->modem, "Device", qmi);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Diag", diag);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_sierra(struct modem_info *modem)
{
	const char *mdm = NULL, *app = NULL, *net = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "01") == 0)
				diag = info->devnode;
			if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "04") == 0)
				app = info->devnode;
			else if (g_strcmp0(info->number, "07") == 0)
				net = info->devnode;
		}
	}

	if (mdm == NULL || net == NULL)
		return FALSE;

	DBG("modem=%s app=%s net=%s diag=%s", mdm, app, net, diag);

	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "App", app);
	ofono_modem_set_string(modem->modem, "Diag", diag);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_option(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				diag = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				aux = info->devnode;
		}

	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s diag=%s", aux, mdm, diag);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Diag", diag);

	return TRUE;
}

static gboolean setup_huawei(struct modem_info *modem)
{
	const char *qmi = NULL, *mdm = NULL, *net = NULL;
	const char *pcui = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "modem") == 0 ||
				g_strcmp0(info->interface, "255/1/1") == 0 ||
				g_strcmp0(info->interface, "255/2/1") == 0 ||
				g_strcmp0(info->interface, "255/1/49") == 0) {
			mdm = info->devnode;
		} else if (g_strcmp0(info->label, "pcui") == 0 ||
				g_strcmp0(info->interface, "255/1/2") == 0 ||
				g_strcmp0(info->interface, "255/2/2") == 0 ||
				g_strcmp0(info->interface, "255/1/50") == 0) {
			pcui = info->devnode;
		} else if (g_strcmp0(info->label, "diag") == 0 ||
				g_strcmp0(info->interface, "255/1/3") == 0 ||
				g_strcmp0(info->interface, "255/2/3") == 0 ||
				g_strcmp0(info->interface, "255/1/51") == 0) {
			diag = info->devnode;
		} else if (g_strcmp0(info->interface, "255/1/8") == 0 ||
				g_strcmp0(info->interface, "255/1/56") == 0) {
			net = info->devnode;
		} else if (g_strcmp0(info->interface, "255/1/9") == 0 ||
				g_strcmp0(info->interface, "255/1/57") == 0) {
			qmi = info->devnode;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				pcui = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				pcui = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				pcui = info->devnode;
			else if (g_strcmp0(info->number, "04") == 0)
				pcui = info->devnode;
		}
	}

	if (qmi != NULL && net != NULL) {
		ofono_modem_set_driver(modem->modem, "gobi");
		goto done;
	}

	if (mdm == NULL || pcui == NULL)
		return FALSE;

done:
	DBG("mdm=%s pcui=%s diag=%s qmi=%s net=%s", mdm, pcui, diag, qmi, net);

	ofono_modem_set_string(modem->modem, "Device", qmi);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Pcui", pcui);
	ofono_modem_set_string(modem->modem, "Diag", diag);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_speedup(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_linktop(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "2/2/1") == 0) {
			if (g_strcmp0(info->number, "01") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_icera(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL, *net = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "2/2/1") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
		} else if (g_strcmp0(info->interface, "2/6/0") == 0) {
			if (g_strcmp0(info->number, "05") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->number, "06") == 0)
				net = info->devnode;
			else if (g_strcmp0(info->number, "07") == 0)
				net = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s net=%s", aux, mdm, net);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "NetworkInterface", net);

	return TRUE;
}

static gboolean setup_alcatel(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "03") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "05") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_novatel(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_nokia(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "10/0/0") == 0) {
			if (g_strcmp0(info->number, "02") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "04") == 0)
				aux = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s", aux, mdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_telit(struct modem_info *modem)
{
	const char *mdm = NULL, *aux = NULL, *gps = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				mdm = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				diag = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				gps = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				aux = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("modem=%s aux=%s gps=%s diag=%s", mdm, aux, gps, diag);

	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "GPS", gps);

	return TRUE;
}

static gboolean setup_simcom(struct modem_info *modem)
{
	const char *mdm = NULL, *aux = NULL, *gps = NULL, *diag = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				diag = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				gps = info->devnode;
			else if (g_strcmp0(info->number, "02") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, "03") == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("modem=%s aux=%s gps=%s diag=%s", mdm, aux, gps, diag);

	ofono_modem_set_string(modem->modem, "Modem", mdm);
	ofono_modem_set_string(modem->modem, "Data", aux);
	ofono_modem_set_string(modem->modem, "GPS", gps);

	return TRUE;
}

static gboolean setup_zte(struct modem_info *modem)
{
	const char *aux = NULL, *mdm = NULL, *qcdm = NULL;
	const char *modem_intf;
	GSList *list;

	DBG("%s", modem->syspath);

	if (g_strcmp0(modem->model, "0016") == 0 ||
				g_strcmp0(modem->model, "0017") == 0 ||
				g_strcmp0(modem->model, "0117") == 0)
		modem_intf = "02";
	else
		modem_intf = "03";

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->label, "aux") == 0) {
			aux = info->devnode;
			if (mdm != NULL)
				break;
		} else if (g_strcmp0(info->label, "modem") == 0) {
			mdm = info->devnode;
			if (aux != NULL)
				break;
		} else if (g_strcmp0(info->interface, "255/255/255") == 0) {
			if (g_strcmp0(info->number, "00") == 0)
				qcdm = info->devnode;
			else if (g_strcmp0(info->number, "01") == 0)
				aux = info->devnode;
			else if (g_strcmp0(info->number, modem_intf) == 0)
				mdm = info->devnode;
		}
	}

	if (aux == NULL || mdm == NULL)
		return FALSE;

	DBG("aux=%s modem=%s qcdm=%s", aux, mdm, qcdm);

	ofono_modem_set_string(modem->modem, "Aux", aux);
	ofono_modem_set_string(modem->modem, "Modem", mdm);

	return TRUE;
}

static gboolean setup_samsung(struct modem_info *modem)
{
	const char *control = NULL, *network = NULL;
	GSList *list;

	DBG("%s", modem->syspath);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s %s %s %s", info->devnode, info->interface,
						info->number, info->label);

		if (g_strcmp0(info->interface, "10/0/0") == 0)
			control = info->devnode;
		else if (g_strcmp0(info->interface, "255/0/0") == 0)
			network = info->devnode;
	}

	if (control == NULL && network == NULL)
		return FALSE;

	DBG("control=%s network=%s", control, network);

	ofono_modem_set_string(modem->modem, "ControlPort", control);
	ofono_modem_set_string(modem->modem, "NetworkInterface", network);

	return TRUE;
}

static struct {
	const char *name;
	gboolean (*setup)(struct modem_info *modem);
	const char *sysattr;
} driver_list[] = {
	{ "isiusb",	setup_isi,	"type"			},
	{ "mbm",	setup_mbm,	"device/interface"	},
	{ "hso",	setup_hso,	"hsotype"		},
	{ "gobi",	setup_gobi	},
	{ "sierra",	setup_sierra	},
	{ "option",	setup_option	},
	{ "huawei",	setup_huawei	},
	{ "speedupcdma",setup_speedup	},
	{ "speedup",	setup_speedup	},
	{ "linktop",	setup_linktop	},
	{ "alcatel",	setup_alcatel	},
	{ "novatel",	setup_novatel	},
	{ "nokia",	setup_nokia	},
	{ "telit",	setup_telit	},
	{ "simcom",	setup_simcom	},
	{ "zte",	setup_zte	},
	{ "icera",	setup_icera	},
	{ "samsung",	setup_samsung	},
	{ }
};

static GHashTable *modem_list;

static const char *get_sysattr(const char *driver)
{
	unsigned int i;

	for (i = 0; driver_list[i].name; i++) {
		if (g_str_equal(driver_list[i].name, driver) == TRUE)
			return driver_list[i].sysattr;
	}

	return NULL;
}

static void destroy_modem(gpointer data)
{
	struct modem_info *modem = data;
	GSList *list;

	DBG("%s", modem->syspath);

	ofono_modem_remove(modem->modem);

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		DBG("%s", info->devnode);

		g_free(info->devpath);
		g_free(info->devnode);
		g_free(info->interface);
		g_free(info->number);
		g_free(info->label);
		g_free(info->sysattr);
		g_free(info);

		list->data = NULL;
	}

	g_slist_free(modem->devices);

	g_free(modem->syspath);
	g_free(modem->devname);
	g_free(modem->driver);
	g_free(modem->vendor);
	g_free(modem->model);
	g_free(modem);
}

static gboolean check_remove(gpointer key, gpointer value, gpointer user_data)
{
	struct modem_info *modem = value;
	const char *devpath = user_data;
	GSList *list;

	for (list = modem->devices; list; list = list->next) {
		struct device_info *info = list->data;

		if (g_strcmp0(info->devpath, devpath) == 0)
			return TRUE;
	}

	return FALSE;
}

static void remove_device(struct udev_device *device)
{
	const char *syspath;

	syspath = udev_device_get_syspath(device);
	if (syspath == NULL)
		return;

	DBG("%s", syspath);

	g_hash_table_foreach_remove(modem_list, check_remove,
						(char *) syspath);
}

static gint compare_device(gconstpointer a, gconstpointer b)
{
	const struct device_info *info1 = a;
	const struct device_info *info2 = b;

	return g_strcmp0(info1->number, info2->number);
}

static void add_device(const char *syspath, const char *devname,
			const char *driver, const char *vendor,
			const char *model, struct udev_device *device)
{
	struct udev_device *intf;
	const char *devpath, *devnode, *interface, *number, *label, *sysattr;
	struct modem_info *modem;
	struct device_info *info;

	devpath = udev_device_get_syspath(device);
	if (devpath == NULL)
		return;

	devnode = udev_device_get_devnode(device);
	if (devnode == NULL) {
		devnode = udev_device_get_property_value(device, "INTERFACE");
		if (devnode == NULL)
			return;
	}

	intf = udev_device_get_parent_with_subsystem_devtype(device,
						"usb", "usb_interface");
	if (intf == NULL)
		return;

	modem = g_hash_table_lookup(modem_list, syspath);
	if (modem == NULL) {
		modem = g_try_new0(struct modem_info, 1);
		if (modem == NULL)
			return;

		modem->syspath = g_strdup(syspath);
		modem->devname = g_strdup(devname);
		modem->driver = g_strdup(driver);
		modem->vendor = g_strdup(vendor);
		modem->model = g_strdup(model);

		modem->sysattr = get_sysattr(driver);

		g_hash_table_replace(modem_list, modem->syspath, modem);
	}

	interface = udev_device_get_property_value(intf, "INTERFACE");
	number = udev_device_get_property_value(device, "ID_USB_INTERFACE_NUM");

	label = udev_device_get_property_value(device, "OFONO_LABEL");

	if (modem->sysattr != NULL)
		sysattr = udev_device_get_sysattr_value(device, modem->sysattr);
	else
		sysattr = NULL;

	DBG("%s", syspath);
	DBG("%s", devpath);
	DBG("%s (%s) %s [%s] ==> %s %s", devnode, driver,
					interface, number, label, sysattr);

	info = g_try_new0(struct device_info, 1);
	if (info == NULL)
		return;

	info->devpath = g_strdup(devpath);
	info->devnode = g_strdup(devnode);
	info->interface = g_strdup(interface);
	info->number = g_strdup(number);
	info->label = g_strdup(label);
	info->sysattr = g_strdup(sysattr);

	modem->devices = g_slist_insert_sorted(modem->devices, info,
							compare_device);
}

static struct {
	const char *driver;
	const char *drv;
	const char *vid;
	const char *pid;
} vendor_list[] = {
	{ "isiusb",	"cdc_phonet"			},
	{ "linktop",	"cdc_acm",	"230d"		},
	{ "icera",	"cdc_acm",	"19d2"		},
	{ "icera",	"cdc_ether",	"19d2"		},
	{ "icera",	"cdc_acm",	"04e8", "6872"	},
	{ "icera",	"cdc_ether",	"04e8", "6872"	},
	{ "icera",	"cdc_acm",	"0421", "0633"	},
	{ "icera",	"cdc_ether",	"0421", "0633"	},
	{ "mbm",	"cdc_acm",	"0bdb"		},
	{ "mbm",	"cdc_ether",	"0bdb"		},
	{ "mbm",	"cdc_acm",	"0fce"		},
	{ "mbm",	"cdc_ether",	"0fce"		},
	{ "mbm",	"cdc_acm",	"413c"		},
	{ "mbm",	"cdc_ether",	"413c"		},
	{ "mbm",	"cdc_acm",	"03f0"		},
	{ "mbm",	"cdc_ether",	"03f0"		},
	{ "mbm",	"cdc_acm",	"0930"		},
	{ "mbm",	"cdc_ether",	"0930"		},
	{ "hso",	"hso"				},
	{ "gobi",	"qmi_wwan"			},
	{ "gobi",	"qcserial"			},
	{ "sierra",	"sierra"			},
	{ "sierra",	"sierra_net"			},
	{ "option",	"option",	"0af0"		},
	{ "huawei",	"option",	"201e"		},
	{ "huawei",	"cdc_wdm",	"12d1"		},
	{ "huawei",	"cdc_ether",	"12d1"		},
	{ "huawei",	"qmi_wwan",	"12d1"		},
	{ "huawei",	"option",	"12d1"		},
	{ "speedupcdma","option",	"1c9e", "9e00"	},
	{ "speedup",	"option",	"1c9e"		},
	{ "speedup",	"option",	"2020"		},
	{ "alcatel",	"option",	"1bbb", "0017"	},
	{ "novatel",	"option",	"1410"		},
	{ "zte",	"option",	"19d2"		},
	{ "simcom",	"option",	"05c6", "9000"	},
	{ "telit",	"usbserial",	"1bc7"		},
	{ "telit",	"option",	"1bc7"		},
	{ "nokia",	"option",	"0421", "060e"	},
	{ "nokia",	"option",	"0421", "0623"	},
	{ "samsung",	"option",	"04e8", "6889"	},
	{ "samsung",	"kalmia"			},
	{ }
};

static void check_usb_device(struct udev_device *device)
{
	struct udev_device *usb_device;
	const char *syspath, *devname, *driver;
	const char *vendor = NULL, *model = NULL;

	usb_device = udev_device_get_parent_with_subsystem_devtype(device,
							"usb", "usb_device");
	if (usb_device == NULL)
		return;

	syspath = udev_device_get_syspath(usb_device);
	if (syspath == NULL)
		return;

	devname = udev_device_get_devnode(usb_device);
	if (devname == NULL)
		return;

	driver = udev_device_get_property_value(usb_device, "OFONO_DRIVER");
	if (driver == NULL) {
		const char *drv, *vid, *pid;
		unsigned int i;

		drv = udev_device_get_property_value(device, "ID_USB_DRIVER");
		if (drv == NULL) {
			drv = udev_device_get_driver(device);
			if (drv == NULL) {
				struct udev_device *parent;

				parent = udev_device_get_parent(device);
				if (parent == NULL)
					return;

				drv = udev_device_get_driver(parent);
				if (drv == NULL)
					return;
			}
		}

		vid = udev_device_get_property_value(device, "ID_VENDOR_ID");
		pid = udev_device_get_property_value(device, "ID_MODEL_ID");

		DBG("%s [%s:%s]", drv, vid, pid);

		for (i = 0; vendor_list[i].driver; i++) {
			if (g_str_equal(vendor_list[i].drv, drv) == FALSE)
				continue;

			if (vendor_list[i].vid == NULL) {
				driver = vendor_list[i].driver;
				vendor = vid;
				model = pid;
				continue;
			}

			if (vid == NULL || pid == NULL)
				continue;

			if (g_str_equal(vendor_list[i].vid, vid) == TRUE) {
				if (vendor_list[i].pid == NULL) {
					driver = vendor_list[i].driver;
					vendor = vid;
					model = pid;
					continue;
				}

				if (g_strcmp0(vendor_list[i].pid, pid) == 0) {
					driver = vendor_list[i].driver;
					vendor = vid;
					model = pid;
					break;
				}
			}
		}

		if (driver == NULL)
			return;
	}

	add_device(syspath, devname, driver, vendor, model, device);
}

static void check_device(struct udev_device *device)
{
	const char *bus;

	bus = udev_device_get_property_value(device, "ID_BUS");
	if (bus == NULL) {
		bus = udev_device_get_subsystem(device);
		if (bus == NULL)
			return;
	}

	if (g_str_equal(bus, "usb") == TRUE)
		check_usb_device(device);
}

static gboolean create_modem(gpointer key, gpointer value, gpointer user_data)
{
	struct modem_info *modem = value;
	const char *syspath = key;
	unsigned int i;

	if (modem->modem != NULL)
		return FALSE;

	DBG("%s", syspath);

	if (modem->devices == NULL)
		return TRUE;

	DBG("driver=%s", modem->driver);

	modem->modem = ofono_modem_create(NULL, modem->driver);
	if (modem->modem == NULL)
		return TRUE;

	for (i = 0; driver_list[i].name; i++) {
		if (g_str_equal(driver_list[i].name, modem->driver) == FALSE)
			continue;

		if (driver_list[i].setup(modem) == TRUE) {
			ofono_modem_register(modem->modem);
			return FALSE;
		}
	}

	return TRUE;
}

static void enumerate_devices(struct udev *context)
{
	struct udev_enumerate *enumerate;
	struct udev_list_entry *entry;

	DBG("");

	enumerate = udev_enumerate_new(context);
	if (enumerate == NULL)
		return;

	udev_enumerate_add_match_subsystem(enumerate, "tty");
	udev_enumerate_add_match_subsystem(enumerate, "usb");
	udev_enumerate_add_match_subsystem(enumerate, "net");

	udev_enumerate_scan_devices(enumerate);

	entry = udev_enumerate_get_list_entry(enumerate);
	while (entry) {
		const char *syspath = udev_list_entry_get_name(entry);
		struct udev_device *device;

		device = udev_device_new_from_syspath(context, syspath);
		if (device != NULL) {
			check_device(device);
			udev_device_unref(device);
		}

		entry = udev_list_entry_get_next(entry);
	}

	udev_enumerate_unref(enumerate);

	g_hash_table_foreach_remove(modem_list, create_modem, NULL);
}

static struct udev *udev_ctx;
static struct udev_monitor *udev_mon;
static guint udev_watch = 0;
static guint udev_delay = 0;

static gboolean check_modem_list(gpointer user_data)
{
	udev_delay = 0;

	DBG("");

	g_hash_table_foreach_remove(modem_list, create_modem, NULL);

	return FALSE;
}

static gboolean udev_event(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct udev_device *device;
	const char *action;

	if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
		ofono_warn("Error with udev monitor channel");
		udev_watch = 0;
		return FALSE;
	}

	device = udev_monitor_receive_device(udev_mon);
	if (device == NULL)
		return TRUE;

	action = udev_device_get_action(device);
	if (action == NULL)
		return TRUE;

	if (g_str_equal(action, "add") == TRUE) {
		if (udev_delay > 0)
			g_source_remove(udev_delay);

		check_device(device);

		udev_delay = g_timeout_add_seconds(1, check_modem_list, NULL);
	} else if (g_str_equal(action, "remove") == TRUE)
		remove_device(device);

	udev_device_unref(device);

	return TRUE;
}

static void udev_start(void)
{
	GIOChannel *channel;
	int fd;

	DBG("");

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

static int detect_init(void)
{
	udev_ctx = udev_new();
	if (udev_ctx == NULL) {
		ofono_error("Failed to create udev context");
		return -EIO;
	}

	udev_mon = udev_monitor_new_from_netlink(udev_ctx, "udev");
	if (udev_mon == NULL) {
		ofono_error("Failed to create udev monitor");
		udev_unref(udev_ctx);
		udev_ctx = NULL;
		return -EIO;
	}

	modem_list = g_hash_table_new_full(g_str_hash, g_str_equal,
						NULL, destroy_modem);

	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "tty", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "usb", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(udev_mon, "net", NULL);

	udev_monitor_filter_update(udev_mon);

	udev_start();

	return 0;
}

static void detect_exit(void)
{
	if (udev_delay > 0)
		g_source_remove(udev_delay);

	if (udev_watch > 0)
		g_source_remove(udev_watch);

	if (udev_ctx == NULL)
		return;

	udev_monitor_filter_remove(udev_mon);

	g_hash_table_destroy(modem_list);

	udev_monitor_unref(udev_mon);
	udev_unref(udev_ctx);
}

OFONO_PLUGIN_DEFINE(udevng, "udev hardware detection", VERSION,
		OFONO_PLUGIN_PRIORITY_DEFAULT, detect_init, detect_exit)
