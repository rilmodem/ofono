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

#include <stdio.h>
#include <string.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sms.h>

#include "qmi.h"
#include "wms.h"

#include "qmimodem.h"

struct sms_data {
	struct qmi_service *wms;
	uint16_t major;
	uint16_t minor;
};

static void get_smsc_addr_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_sca_query_cb_t cb = cbd->cb;
	struct ofono_phone_number sca;
	const struct qmi_wms_result_smsc_addr *smsc;
	uint16_t len;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	smsc = qmi_result_get(result, QMI_WMS_RESULT_SMSC_ADDR, &len);
	if (!smsc) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	if (!smsc->addr_len) {
		CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);
		return;
	}

	if (smsc->addr[0] == '+') {
		strncpy(sca.number, smsc->addr + 1, smsc->addr_len - 1);
		sca.number[smsc->addr_len - 1] = '\0';
		sca.type = 145;
	} else {
		strncpy(sca.number, smsc->addr, smsc->addr_len);
		sca.number[smsc->addr_len] = '\0';
		sca.type = 129;
	}

	CALLBACK_WITH_SUCCESS(cb, &sca, cbd->data);
}

static void qmi_sca_query(struct ofono_sms *sms,
				ofono_sms_sca_query_cb_t cb, void *user_data)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->wms, QMI_WMS_GET_SMSC_ADDR, NULL,
					get_smsc_addr_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, NULL, cbd->data);

	g_free(cbd);
}

static void set_smsc_addr_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_sca_set_cb_t cb = cbd->cb;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void qmi_sca_set(struct ofono_sms *sms,
				const struct ofono_phone_number *sca,
				ofono_sms_sca_set_cb_t cb, void *user_data)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	char type[4], number[OFONO_MAX_PHONE_NUMBER_LENGTH + 2];
	struct qmi_param *param;

	DBG("type %d name %s", sca->type, sca->number);

	switch (sca->type) {
	case 129:
		snprintf(number, sizeof(number), "%s", sca->number);
		break;
	case 145:
		snprintf(number, sizeof(number), "+%s", sca->number);
		break;
	default:
		goto error;
	}

	snprintf(type, sizeof(type), "%d", sca->type);

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, QMI_WMS_PARAM_SMSC_ADDR,
						strlen(number), number);
	qmi_param_append(param, QMI_WMS_PARAM_SMSC_ADDR_TYPE,
						strlen(type), type);

	if (qmi_service_send(data->wms, QMI_WMS_SET_SMSC_ADDR, param,
					set_smsc_addr_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void raw_send_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_submit_cb_t cb = cbd->cb;
	uint16_t msgid;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	if (!qmi_result_get_uint16(result, QMI_WMS_RESULT_MESSAGE_ID, &msgid)) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, msgid, cbd->data);
}

static void qmi_submit(struct ofono_sms *sms,
			const unsigned char *pdu, int pdu_len, int tpdu_len,
			int mms, ofono_sms_submit_cb_t cb, void *user_data)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_wms_param_message *message;
	struct qmi_param *param;

	DBG("pdu_len %d tpdu_len %d mms %d", pdu_len, tpdu_len, mms);

	message = alloca(3 + pdu_len);

	message->msg_format = 0x06;
	message->msg_length = GUINT16_TO_LE(pdu_len);
	memcpy(message->msg_data, pdu, pdu_len);

	param = qmi_param_new();
	if (!param)
		goto error;

	qmi_param_append(param, QMI_WMS_PARAM_MESSAGE, 3 + pdu_len, message);

	if (qmi_service_send(data->wms, QMI_WMS_RAW_SEND, param,
					raw_send_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

	g_free(cbd);
}

static int domain_to_bearer(uint8_t domain)
{
	switch (domain) {
	case QMI_WMS_DOMAIN_CS_PREFERRED:
		return 3;
	case QMI_WMS_DOMAIN_PS_PREFERRED:
		return 2;
	case QMI_WMS_DOMAIN_CS_ONLY:
		return 1;
	case QMI_WMS_DOMAIN_PS_ONLY:
		return 0;
	}

	return -1;
}

static uint8_t bearer_to_domain(int bearer)
{
	switch (bearer) {
	case 0:
		return QMI_WMS_DOMAIN_PS_ONLY;
	case 1:
		return QMI_WMS_DOMAIN_CS_ONLY;
	case 2:
		return QMI_WMS_DOMAIN_PS_PREFERRED;
	case 3:
		return QMI_WMS_DOMAIN_CS_PREFERRED;
	}

	return QMI_WMS_DOMAIN_CS_PREFERRED;
}

static void get_domain_pref_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_bearer_query_cb_t cb = cbd->cb;
	uint8_t domain;
	int bearer;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	if (!qmi_result_get_uint8(result, QMI_WMS_RESULT_DOMAIN, &domain)) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	bearer = domain_to_bearer(domain);

	CALLBACK_WITH_SUCCESS(cb, bearer, cbd->data);
}

static void qmi_bearer_query(struct ofono_sms *sms,
				ofono_sms_bearer_query_cb_t cb, void *user_data)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (data->major < 1 && data->minor < 2)
		goto error;

	if (qmi_service_send(data->wms, QMI_WMS_GET_DOMAIN_PREF, NULL,
					get_domain_pref_cb, cbd, g_free) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

	g_free(cbd);
}

static void set_domain_pref_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_sms_bearer_set_cb_t cb = cbd->cb;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void qmi_bearer_set(struct ofono_sms *sms, int bearer,
				ofono_sms_bearer_set_cb_t cb, void *user_data)
{
	struct sms_data *data = ofono_sms_get_data(sms);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_param *param;
	uint8_t domain;

	DBG("bearer %d", bearer);

	if (data->major < 1 && data->minor < 2)
		goto error;

	domain = bearer_to_domain(bearer);

	param = qmi_param_new_uint8(QMI_WMS_PARAM_DOMAIN, domain);
	if (!param)
		goto error;

	if (qmi_service_send(data->wms, QMI_WMS_SET_DOMAIN_PREF, param,
					set_domain_pref_cb, cbd, g_free) > 0)
		return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void event_notify(struct qmi_result *result, void *user_data)
{
	struct ofono_sms *sms = user_data;
	const struct qmi_wms_result_new_msg_notify *notify;
	const struct qmi_wms_result_message *message;
	uint16_t len;

	DBG("");

	notify = qmi_result_get(result, QMI_WMS_RESULT_NEW_MSG_NOTIFY, &len);
	if (notify) {
		DBG("storage type %d index %d", notify->storage_type,
				GUINT32_FROM_LE(notify->storage_index));
	}

	message = qmi_result_get(result, QMI_WMS_RESULT_MESSAGE, &len);
	if (message) {
		uint16_t plen;

		plen = GUINT16_FROM_LE(message->msg_length);

		DBG("ack_required %d transaction id %u", message->ack_required,
				GUINT32_FROM_LE(message->transaction_id));
		DBG("msg format %d PDU length %d", message->msg_format, plen);

		ofono_sms_deliver_notify(sms, message->msg_data, plen, plen);
	}
}

static void set_routes_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_sms *sms = user_data;

	DBG("");

	ofono_sms_register(sms);
}

static void get_routes_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);
	const struct qmi_wms_route_list *list;
	struct qmi_wms_route_list *new_list;
	struct qmi_param *param;
	uint16_t len, num, i;
	uint8_t value;

	DBG("");

	if (qmi_result_set_error(result, NULL))
		goto done;

	list = qmi_result_get(result, QMI_WMS_RESULT_ROUTE_LIST, &len);
        if (!list)
		goto done;

	num = GUINT16_FROM_LE(list->count);

	DBG("found %d routes", num);

	for (i = 0; i < num; i++)
		DBG("type %d class %d => type %d value %d",
					list->route[i].msg_type,
					list->route[i].msg_class,
					list->route[i].storage_type,
					list->route[i].action);

	if (qmi_result_get_uint8(result, QMI_WMS_RESULT_STATUS_REPORT, &value))
		DBG("transfer status report %d", value);

	len = 2 + (1 * 4);
	new_list = alloca(len);

	new_list->count = GUINT16_TO_LE(1);
	new_list->route[0].msg_type = QMI_WMS_MSG_TYPE_P2P;
	new_list->route[0].msg_class = QMI_WMS_MSG_CLASS_NONE;
	new_list->route[0].storage_type = QMI_WMS_STORAGE_TYPE_NV;
	new_list->route[0].action = QMI_WMS_ACTION_TRANSFER_AND_ACK;

	param = qmi_param_new();
	if (!param)
		goto done;

	qmi_param_append(param, QMI_WMS_PARAM_ROUTE_LIST, len, new_list);
	qmi_param_append_uint8(param, QMI_WMS_PARAM_STATUS_REPORT, 0x01);

        if (qmi_service_send(data->wms, QMI_WMS_SET_ROUTES, param,
                                        set_routes_cb, sms, NULL) > 0)
                return;

	qmi_param_free(param);

done:
	ofono_sms_register(sms);
}

static void set_event_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);

	DBG("");

	if (qmi_service_send(data->wms, QMI_WMS_GET_ROUTES, NULL,
					get_routes_cb, sms, NULL) > 0)
		return;

	ofono_sms_register(sms);
}

static void create_wms_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_sms *sms = user_data;
	struct sms_data *data = ofono_sms_get_data(sms);
	struct qmi_param *param;

	DBG("");

	if (!service) {
		ofono_error("Failed to request WMS service");
		ofono_sms_remove(sms);
		return;
	}

	if (!qmi_service_get_version(service, &data->major, &data->minor)) {
		ofono_error("Failed to get WMS service version");
		ofono_sms_remove(sms);
		return;
	}

	data->wms = qmi_service_ref(service);

	qmi_service_register(data->wms, QMI_WMS_EVENT,
					event_notify, sms, NULL);

	param = qmi_param_new_uint8(QMI_WMS_PARAM_NEW_MSG_REPORT, 0x01);
	if (!param)
		goto done;

	if (qmi_service_send(data->wms, QMI_WMS_SET_EVENT, param,
					set_event_cb, sms, NULL) > 0)
		return;

done:
	ofono_sms_register(sms);
}

static int qmi_sms_probe(struct ofono_sms *sms,
				unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct sms_data *data;

	DBG("");

	data = g_new0(struct sms_data, 1);

	ofono_sms_set_data(sms, data);

	qmi_service_create(device, QMI_SERVICE_WMS, create_wms_cb, sms, NULL);

	return 0;
}

static void qmi_sms_remove(struct ofono_sms *sms)
{
	struct sms_data *data = ofono_sms_get_data(sms);

	DBG("");

	ofono_sms_set_data(sms, NULL);

	qmi_service_unregister_all(data->wms);

	qmi_service_unref(data->wms);

	g_free(data);
}

static struct ofono_sms_driver driver = {
	.name		= "qmimodem",
	.probe		= qmi_sms_probe,
	.remove		= qmi_sms_remove,
	.sca_query	= qmi_sca_query,
	.sca_set	= qmi_sca_set,
	.submit		= qmi_submit,
	.bearer_query	= qmi_bearer_query,
	.bearer_set	= qmi_bearer_set,
};

void qmi_sms_init(void)
{
	ofono_sms_driver_register(&driver);
}

void qmi_sms_exit(void)
{
	ofono_sms_driver_unregister(&driver);
}
