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
#include <ofono/ussd.h>

#include "qmi.h"

#include "qmimodem.h"

struct ussd_data {
	struct qmi_service *voice;
	uint16_t major;
	uint16_t minor;
};

static void create_voice_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_ussd *ussd = user_data;
	struct ussd_data *data = ofono_ussd_get_data(ussd);

	DBG("");

	if (!service) {
		ofono_error("Failed to request Voice service");
		ofono_ussd_remove(ussd);
		return;
	}

	if (!qmi_service_get_version(service, &data->major, &data->minor)) {
		ofono_error("Failed to get Voice service version");
		ofono_ussd_remove(ussd);
		return;
	}

	data->voice = qmi_service_ref(service);

	ofono_ussd_register(ussd);
}

static int qmi_ussd_probe(struct ofono_ussd *ussd,
				unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct ussd_data *data;

	DBG("");

	data = g_new0(struct ussd_data, 1);

	ofono_ussd_set_data(ussd, data);

	qmi_service_create_shared(device, QMI_SERVICE_VOICE,
						create_voice_cb, ussd, NULL);

	return 0;

}

static void qmi_ussd_remove(struct ofono_ussd *ussd)
{
	struct ussd_data *data = ofono_ussd_get_data(ussd);

	DBG("");

	ofono_ussd_set_data(ussd, NULL);

	qmi_service_unref(data->voice);

	g_free(data);
}

static struct ofono_ussd_driver driver = {
	.name		= "qmimodem",
	.probe		= qmi_ussd_probe,
	.remove		= qmi_ussd_remove,
};

void qmi_ussd_init(void)
{
	ofono_ussd_driver_register(&driver);
}

void qmi_ussd_exit(void)
{
	ofono_ussd_driver_unregister(&driver);
}
