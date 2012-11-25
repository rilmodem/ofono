/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
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
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/devinfo.h>
#include <ofono/netreg.h>
#include <ofono/phonebook.h>
#include <ofono/voicecall.h>
#include <ofono/sim.h>
#include <ofono/stk.h>
#include <ofono/sms.h>
#include <ofono/ussd.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/location-reporting.h>
#include <ofono/log.h>

#include <drivers/qmimodem/qmi.h>
#include <drivers/qmimodem/dms.h>
#include <drivers/qmimodem/util.h>

#define GOBI_DMS	(1 << 0)
#define GOBI_NAS	(1 << 1)
#define GOBI_WMS	(1 << 2)
#define GOBI_WDS	(1 << 3)
#define GOBI_PDS	(1 << 4)
#define GOBI_PBM	(1 << 5)
#define GOBI_UIM	(1 << 6)
#define GOBI_CAT	(1 << 7)
#define GOBI_CAT_OLD	(1 << 8)
#define GOBI_VOICE	(1 << 9)

struct gobi_data {
	struct qmi_device *device;
	struct qmi_service *dms;
	unsigned long features;
	unsigned int discover_attempts;
	uint8_t oper_mode;
};

static void gobi_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	ofono_info("%s%s", prefix, str);
}

static int gobi_probe(struct ofono_modem *modem)
{
	struct gobi_data *data;

	DBG("%p", modem);

	data = g_try_new0(struct gobi_data, 1);
	if (!data)
		return -ENOMEM;

	ofono_modem_set_data(modem, data);

	return 0;
}

static void gobi_remove(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_modem_set_data(modem, NULL);

	qmi_service_unref(data->dms);

	qmi_device_unref(data->device);

	g_free(data);
}

static void shutdown_cb(void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("");

	data->discover_attempts = 0;

	qmi_device_unref(data->device);
	data->device = NULL;

	ofono_modem_set_powered(modem, FALSE);
}

static void shutdown_device(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	qmi_service_unref(data->dms);
	data->dms = NULL;

	qmi_device_shutdown(data->device, shutdown_cb, modem, NULL);
}

static void power_reset_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		shutdown_device(modem);
		return;
	}

	ofono_modem_set_powered(modem, TRUE);
}

static void get_oper_mode_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct gobi_data *data = ofono_modem_get_data(modem);
	struct qmi_param *param;
	uint8_t mode;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		shutdown_device(modem);
		return;
	}

	if (!qmi_result_get_uint8(result, QMI_DMS_RESULT_OPER_MODE, &mode)) {
		shutdown_device(modem);
		return;
	}

	data->oper_mode = mode;

	switch (data->oper_mode) {
	case QMI_DMS_OPER_MODE_ONLINE:
		param = qmi_param_new_uint8(QMI_DMS_PARAM_OPER_MODE,
					QMI_DMS_OPER_MODE_PERSIST_LOW_POWER);
		if (!param) {
			shutdown_device(modem);
			return;
		}

		if (qmi_service_send(data->dms, QMI_DMS_SET_OPER_MODE, param,
					power_reset_cb, modem, NULL) > 0)
			return;

		shutdown_device(modem);
		break;
	default:
		ofono_modem_set_powered(modem, TRUE);
		break;
	}
}

static void get_caps_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct gobi_data *data = ofono_modem_get_data(modem);
	const struct qmi_dms_device_caps *caps;
	uint16_t len;
	uint8_t i;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	caps = qmi_result_get(result, QMI_DMS_RESULT_DEVICE_CAPS, &len);
	if (!caps)
		goto error;

        DBG("service capabilities %d", caps->data_capa);
        DBG("sim supported %d", caps->sim_supported);

        for (i = 0; i < caps->radio_if_count; i++)
                DBG("radio = %d", caps->radio_if[i]);

	if (qmi_service_send(data->dms, QMI_DMS_GET_OPER_MODE, NULL,
					get_oper_mode_cb, modem, NULL) > 0)
		return;

error:
	shutdown_device(modem);
}

static void create_dms_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("");

	if (!service)
		goto error;

	data->dms = qmi_service_ref(service);

	if (qmi_service_send(data->dms, QMI_DMS_GET_CAPS, NULL,
					get_caps_cb, modem, NULL) > 0)
		return;

error:
	shutdown_device(modem);
}

static void discover_cb(uint8_t count, const struct qmi_version *list,
							void *user_data)
{
	struct ofono_modem *modem = user_data;
	struct gobi_data *data = ofono_modem_get_data(modem);
	uint8_t i;

	DBG("");

	for (i = 0; i < count; i++) {
		DBG("%s %d.%d", list[i].name, list[i].major, list[i].minor);

		switch (list[i].type) {
		case QMI_SERVICE_DMS:
			data->features |= GOBI_DMS;
			break;
		case QMI_SERVICE_NAS:
			data->features |= GOBI_NAS;
			break;
		case QMI_SERVICE_WMS:
			data->features |= GOBI_WMS;
			break;
		case QMI_SERVICE_WDS:
			data->features |= GOBI_WDS;
			break;
		case QMI_SERVICE_PDS:
			data->features |= GOBI_PDS;
			break;
		case QMI_SERVICE_PBM:
			data->features |= GOBI_PBM;
			break;
		case QMI_SERVICE_UIM:
			data->features |= GOBI_UIM;
			break;
		case QMI_SERVICE_CAT:
			data->features |= GOBI_CAT;
			break;
		case QMI_SERVICE_CAT_OLD:
			if (list[i].major > 0)
				data->features |= GOBI_CAT_OLD;
			break;
		case QMI_SERVICE_VOICE:
			data->features |= GOBI_VOICE;
			break;
		}
	}

	if (!(data->features & GOBI_DMS)) {
		if (++data->discover_attempts < 3) {
			qmi_device_discover(data->device, discover_cb,
								modem, NULL);
			return;
		}

		shutdown_device(modem);
		return;
	}

	qmi_service_create_shared(data->device, QMI_SERVICE_DMS,
						create_dms_cb, modem, NULL);
}

static int gobi_enable(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);
	const char *device;
	int fd;

	DBG("%p", modem);

	device = ofono_modem_get_string(modem, "Device");
	if (!device)
		return -EINVAL;

	fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -EIO;

	data->device = qmi_device_new(fd);
	if (!data->device) {
		close(fd);
		return -ENOMEM;
	}

	if (getenv("OFONO_QMI_DEBUG"))
		qmi_device_set_debug(data->device, gobi_debug, "QMI: ");

	qmi_device_set_close_on_unref(data->device, true);

	qmi_device_discover(data->device, discover_cb, modem, NULL);

	return -EINPROGRESS;
}

static void power_disable_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_modem *modem = user_data;

	DBG("");

	shutdown_device(modem);
}

static int gobi_disable(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);
	struct qmi_param *param;

	DBG("%p", modem);

	qmi_service_cancel_all(data->dms);
	qmi_service_unregister_all(data->dms);

	param = qmi_param_new_uint8(QMI_DMS_PARAM_OPER_MODE,
					QMI_DMS_OPER_MODE_PERSIST_LOW_POWER);
	if (!param)
		return -ENOMEM;

	if (qmi_service_send(data->dms, QMI_DMS_SET_OPER_MODE, param,
					power_disable_cb, modem, NULL) > 0)
		return -EINPROGRESS;

	shutdown_device(modem);

	return -EINPROGRESS;
}

static void set_online_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_modem_online_cb_t cb = cbd->cb;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	else
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void gobi_set_online(struct ofono_modem *modem, ofono_bool_t online,
				ofono_modem_online_cb_t cb, void *user_data)
{
	struct gobi_data *data = ofono_modem_get_data(modem);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_param *param;
	uint8_t mode;

	DBG("%p %s", modem, online ? "online" : "offline");

	if (online)
		mode = QMI_DMS_OPER_MODE_ONLINE;
	else
		mode = QMI_DMS_OPER_MODE_LOW_POWER;

	param = qmi_param_new_uint8(QMI_DMS_PARAM_OPER_MODE, mode);
	if (!param)
		goto error;

	if (qmi_service_send(data->dms, QMI_DMS_SET_OPER_MODE, param,
					set_online_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void gobi_pre_sim(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	ofono_devinfo_create(modem, 0, "qmimodem", data->device);

	if (data->features & GOBI_UIM)
		ofono_sim_create(modem, 0, "qmimodem", data->device);
	else if (data->features & GOBI_DMS)
		ofono_sim_create(modem, 0, "qmimodem-legacy", data->device);

	if (data->features & GOBI_VOICE)
		ofono_voicecall_create(modem, 0, "qmimodem", data->device);

	if (data->features & GOBI_PDS)
		ofono_location_reporting_create(modem, 0, "qmimodem",
							data->device);
}

static void gobi_post_sim(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);

	DBG("%p", modem);

	if (data->features & GOBI_CAT)
		ofono_stk_create(modem, 0, "qmimodem", data->device);
	else if (data->features & GOBI_CAT_OLD)
		ofono_stk_create(modem, 1, "qmimodem", data->device);

	if (data->features & GOBI_PBM)
		ofono_phonebook_create(modem, 0, "qmimodem", data->device);

	if (data->features & GOBI_NAS)
		ofono_radio_settings_create(modem, 0, "qmimodem", data->device);

	if (data->features & GOBI_WMS)
		ofono_sms_create(modem, 0, "qmimodem", data->device);
}

static void gobi_post_online(struct ofono_modem *modem)
{
	struct gobi_data *data = ofono_modem_get_data(modem);
	struct ofono_gprs *gprs;
	struct ofono_gprs_context *gc;

	DBG("%p", modem);

	if (data->features & GOBI_NAS)
		ofono_netreg_create(modem, 0, "qmimodem", data->device);

	if (data->features & GOBI_VOICE)
		ofono_ussd_create(modem, 0, "qmimodem", data->device);

	if (data->features & GOBI_WDS) {
		gprs = ofono_gprs_create(modem, 0, "qmimodem", data->device);
		gc = ofono_gprs_context_create(modem, 0, "qmimodem",
							data->device);

		if (gprs && gc)
			ofono_gprs_add_context(gprs, gc);
	}
}

static struct ofono_modem_driver gobi_driver = {
	.name		= "gobi",
	.probe		= gobi_probe,
	.remove		= gobi_remove,
	.enable		= gobi_enable,
	.disable	= gobi_disable,
	.set_online	= gobi_set_online,
	.pre_sim	= gobi_pre_sim,
	.post_sim	= gobi_post_sim,
	.post_online	= gobi_post_online,
};

static int gobi_init(void)
{
	return ofono_modem_driver_register(&gobi_driver);
}

static void gobi_exit(void)
{
	ofono_modem_driver_unregister(&gobi_driver);
}

OFONO_PLUGIN_DEFINE(gobi, "Qualcomm Gobi modem driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, gobi_init, gobi_exit)
