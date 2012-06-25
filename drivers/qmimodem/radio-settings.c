/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/radio-settings.h>

#include "qmi.h"
#include "nas.h"

#include "qmimodem.h"

struct settings_data {
	struct qmi_service *nas;
	uint16_t major;
	uint16_t minor;
};

static void create_nas_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_radio_settings *rs = user_data;
	struct settings_data *data = ofono_radio_settings_get_data(rs);

	DBG("");

	if (!service) {
		ofono_error("Failed to request NAS service");
		ofono_radio_settings_remove(rs);
		return;
	}

	if (!qmi_service_get_version(service, &data->major, &data->minor)) {
		ofono_error("Failed to get NAS service version");
		ofono_radio_settings_remove(rs);
		return;
	}

	data->nas = qmi_service_ref(service);

	ofono_radio_settings_register(rs);
}

static int qmi_radio_settings_probe(struct ofono_radio_settings *rs,
					unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct settings_data *data;

	DBG("");

	data = g_new0(struct settings_data, 1);

	ofono_radio_settings_set_data(rs, data);

	qmi_service_create(device, QMI_SERVICE_NAS, create_nas_cb, rs, NULL);

	return 0;

}

static void qmi_radio_settings_remove(struct ofono_radio_settings *rs)
{
	struct settings_data *data = ofono_radio_settings_get_data(rs);

	DBG("");

	ofono_radio_settings_set_data(rs, NULL);

	qmi_service_unregister_all(data->nas);

	qmi_service_unref(data->nas);

	g_free(data);
}

static struct ofono_radio_settings_driver driver = {
	.name		= "qmimodem",
	.probe		= qmi_radio_settings_probe,
	.remove		= qmi_radio_settings_remove,
};

void qmi_radio_settings_init(void)
{
	ofono_radio_settings_driver_register(&driver);
}

void qmi_radio_settings_exit(void)
{
	ofono_radio_settings_driver_unregister(&driver);
}
