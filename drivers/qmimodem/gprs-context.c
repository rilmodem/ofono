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

#include <string.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs-context.h>

#include "qmi.h"
#include "wds.h"

#include "qmimodem.h"

struct gprs_context_data {
	struct qmi_service *wds;
	unsigned int active_context;
	uint32_t pkt_handle;
};

static void pkt_status_notify(struct qmi_result *result, void *user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *data = ofono_gprs_context_get_data(gc);
	const struct qmi_wds_notify_conn_status *status;
	uint16_t len;
	uint8_t ip_family;

	DBG("");

	status = qmi_result_get(result, QMI_WDS_NOTIFY_CONN_STATUS, &len);
	if (!status)
		return;

	DBG("conn status %d", status->status);

	if (qmi_result_get_uint8(result, QMI_WDS_NOTIFY_IP_FAMILY, &ip_family))
		DBG("ip family %d", ip_family);

	switch (status->status) {
	case QMI_WDS_CONN_STATUS_DISCONNECTED:
		ofono_gprs_context_deactivated(gc, data->active_context);
		data->active_context = 0;
		break;
	}
}

static void get_settings_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct ofono_modem *modem;
	const char *interface;
	uint8_t pdp_type, ip_family;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto done;

	if (qmi_result_get_uint8(result, QMI_WDS_RESULT_PDP_TYPE, &pdp_type))
		DBG("PDP type %d", pdp_type);

	if (qmi_result_get_uint8(result, QMI_WDS_RESULT_IP_FAMILY, &ip_family))
		DBG("IP family %d", ip_family);

done:
	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");

	ofono_gprs_context_set_interface(gc, interface);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

	g_free(cbd);
}

static void start_net_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *data = ofono_gprs_context_get_data(gc);
	struct ofono_modem *modem;
	const char *interface;
	uint32_t handle;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto error;

	if (!qmi_result_get_uint32(result, QMI_WDS_RESULT_PKT_HANDLE, &handle))
		goto error;

	DBG("packet handle %d", handle);

	data->pkt_handle = handle;

	if (qmi_service_send(data->wds, QMI_WDS_GET_SETTINGS, NULL,
					get_settings_cb, cbd, NULL) > 0)
		return;

	modem = ofono_gprs_context_get_modem(gc);
	interface = ofono_modem_get_string(modem, "NetworkInterface");

	ofono_gprs_context_set_interface(gc, interface);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

	g_free(cbd);

	return;

error:
	data->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void qmi_activate_primary(struct ofono_gprs_context *gc,
				const struct ofono_gprs_primary_context *ctx,
				ofono_gprs_context_cb_t cb, void *user_data)
{
	struct gprs_context_data *data = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_param *param;
	uint8_t ip_family;

	DBG("cid %u", ctx->cid);

	cbd->user = gc;

	data->active_context = ctx->cid;

	switch (ctx->proto) {
	case OFONO_GPRS_PROTO_IP:
		ip_family = 4;
		break;
	case OFONO_GPRS_PROTO_IPV6:
		ip_family = 6;
		break;
	default:
		goto error;
	}

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, QMI_WDS_PARAM_APN,
					strlen(ctx->apn), ctx->apn);

	qmi_param_append_uint8(param, QMI_WDS_PARAM_IP_FAMILY, ip_family);

	if (qmi_service_send(data->wds, QMI_WDS_START_NET, param,
					start_net_cb, cbd, NULL) > 0)
		return;

	qmi_param_free(param);

error:
	data->active_context = 0;

	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void stop_net_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_context_cb_t cb = cbd->cb;
	struct ofono_gprs_context *gc = cbd->user;
	struct gprs_context_data *data = ofono_gprs_context_get_data(gc);

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	data->active_context = 0;

	data->pkt_handle = 0;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);

	g_free(cbd);
}

static void qmi_deactivate_primary(struct ofono_gprs_context *gc,
				unsigned int cid,
				ofono_gprs_context_cb_t cb, void *user_data)
{
	struct gprs_context_data *data = ofono_gprs_context_get_data(gc);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_param *param;

	DBG("cid %u", cid);

	cbd->user = gc;

	param = qmi_param_new_uint32(QMI_WDS_PARAM_PKT_HANDLE,
						data->pkt_handle);
	if (!param)
		goto error;

	if (qmi_service_send(data->wds, QMI_WDS_STOP_NET, param,
					stop_net_cb, cbd, NULL) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void create_wds_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_gprs_context *gc = user_data;
	struct gprs_context_data *data = ofono_gprs_context_get_data(gc);

	DBG("");

	if (!service) {
		ofono_error("Failed to request WDS service");
		ofono_gprs_context_remove(gc);
		return;
	}

	data->wds = qmi_service_ref(service);

	qmi_service_register(data->wds, QMI_WDS_PKT_STATUS_IND,
					pkt_status_notify, gc, NULL);
}

static int qmi_gprs_context_probe(struct ofono_gprs_context *gc,
					unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct gprs_context_data *data;

	DBG("");

	data = g_new0(struct gprs_context_data, 1);

	ofono_gprs_context_set_data(gc, data);

	qmi_service_create(device, QMI_SERVICE_WDS, create_wds_cb, gc, NULL);

	return 0;
}

static void qmi_gprs_context_remove(struct ofono_gprs_context *gc)
{
	struct gprs_context_data *data = ofono_gprs_context_get_data(gc);

	DBG("");

	ofono_gprs_context_set_data(gc, NULL);

	qmi_service_unregister_all(data->wds);

	qmi_service_unref(data->wds);

	g_free(data);
}

static struct ofono_gprs_context_driver driver = {
	.name			= "qmimodem",
	.probe			= qmi_gprs_context_probe,
	.remove			= qmi_gprs_context_remove,
	.activate_primary	= qmi_activate_primary,
	.deactivate_primary	= qmi_deactivate_primary,
};

void qmi_gprs_context_init(void)
{
	ofono_gprs_context_driver_register(&driver);
}

void qmi_gprs_context_exit(void)
{
	ofono_gprs_context_driver_unregister(&driver);
}
