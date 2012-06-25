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
#include <ofono/voicecall.h>

#include "qmi.h"

#include "qmimodem.h"

struct voicecall_data {
	struct qmi_service *voice;
	uint16_t major;
	uint16_t minor;
};

static void create_voice_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_voicecall *vc = user_data;
	struct voicecall_data *data = ofono_voicecall_get_data(vc);

	DBG("");

	if (!service) {
		ofono_error("Failed to request Voice service");
		ofono_voicecall_remove(vc);
		return;
	}

	if (!qmi_service_get_version(service, &data->major, &data->minor)) {
		ofono_error("Failed to get Voice service version");
		ofono_voicecall_remove(vc);
		return;
	}

	data->voice = qmi_service_ref(service);

	ofono_voicecall_register(vc);
}

static int qmi_voicecall_probe(struct ofono_voicecall *vc,
					unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct voicecall_data *data;

	DBG("");

	data = g_new0(struct voicecall_data, 1);

	ofono_voicecall_set_data(vc, data);

	qmi_service_create(device, QMI_SERVICE_VOICE,
					create_voice_cb, vc, NULL);

	return 0;

}

static void qmi_voicecall_remove(struct ofono_voicecall *vc)
{
	struct voicecall_data *data = ofono_voicecall_get_data(vc);

	DBG("");

	ofono_voicecall_set_data(vc, NULL);

	qmi_service_unregister_all(data->voice);

	qmi_service_unref(data->voice);

	g_free(data);
}

static struct ofono_voicecall_driver driver = {
	.name		= "qmimodem",
	.probe		= qmi_voicecall_probe,
	.remove		= qmi_voicecall_remove,
};

void qmi_voicecall_init(void)
{
	ofono_voicecall_driver_register(&driver);
}

void qmi_voicecall_exit(void)
{
	ofono_voicecall_driver_unregister(&driver);
}
