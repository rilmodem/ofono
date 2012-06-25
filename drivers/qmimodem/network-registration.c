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
#include <stdlib.h>
#include <string.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>

#include "qmi.h"
#include "nas.h"

#include "qmimodem.h"
#include "src/common.h"

struct netreg_data {
	struct qmi_service *nas;
	struct ofono_network_operator operator;
	uint8_t current_rat;
};

static int rat_to_tech(uint8_t rat)
{
	switch (rat) {
	case QMI_NAS_NETWORK_RAT_GSM:
		return ACCESS_TECHNOLOGY_GSM;
	case QMI_NAS_NETWORK_RAT_UMTS:
		return ACCESS_TECHNOLOGY_UTRAN;
	case QMI_NAS_NETWORK_RAT_LTE:
		return ACCESS_TECHNOLOGY_EUTRAN;
	}

	return -1;
}

static bool extract_ss_info(struct qmi_result *result, int *status,
				int *lac, int *cellid, int *tech,
				struct ofono_network_operator *operator)
{
	const struct qmi_nas_serving_system *ss;
	const struct qmi_nas_current_plmn *plmn;
	uint8_t i, roaming;
	uint16_t value16, len;
	uint32_t value32;

	DBG("");

	ss = qmi_result_get(result, QMI_NAS_RESULT_SERVING_SYSTEM, &len);
	if (!ss)
		return false;

	*status = ss->status;

	DBG("serving system status %d", ss->status);

	*tech = -1;

	for (i = 0; i < ss->radio_if_count; i++) {
		DBG("radio in use %d", ss->radio_if[i]);

		*tech = rat_to_tech(ss->radio_if[i]);
	}

	if (qmi_result_get_uint8(result, QMI_NAS_RESULT_ROAMING_STATUS,
								&roaming)) {
		if (ss->status == 1 && roaming == 0)
			*status = 5;
	}

	if (!operator)
		return true;

	plmn = qmi_result_get(result, QMI_NAS_RESULT_CURRENT_PLMN, &len);
	if (plmn) {
		snprintf(operator->mcc, OFONO_MAX_MCC_LENGTH + 1, "%03d",
						GUINT16_FROM_LE(plmn->mcc));
		snprintf(operator->mnc, OFONO_MAX_MNC_LENGTH + 1, "%02d",
						GUINT16_FROM_LE(plmn->mnc));
		strncpy(operator->name, plmn->desc, plmn->desc_len);
		operator->name[plmn->desc_len] = '\0';

		DBG("%s (%s:%s)", operator->name, operator->mcc, operator->mnc);
	}

	if (qmi_result_get_uint16(result, QMI_NAS_RESULT_LOCATION_AREA_CODE,
								&value16))
		*lac = value16;
	else
		*lac = -1;

	if (qmi_result_get_uint32(result, QMI_NAS_RESULT_CELL_ID, &value32))
		*cellid = value32;
	else
		*cellid = -1;

	DBG("lac %d cellid %d tech %d", *lac, *cellid, *tech);

	return true;
}

static void ss_info_notify(struct qmi_result *result, void *user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	int status, lac, cellid, tech;

	DBG("");

	if (!extract_ss_info(result, &status, &lac, &cellid, &tech,
							&data->operator))
		return;

	ofono_netreg_status_notify(netreg, status, lac, cellid, tech);
}

static void get_ss_info_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_status_cb_t cb = cbd->cb;
	struct netreg_data *data = cbd->user;
	int status, lac, cellid, tech;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
		return;
	}

	if (!extract_ss_info(result, &status, &lac, &cellid, &tech,
							&data->operator)) {
		CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);
		return;
	}

	CALLBACK_WITH_SUCCESS(cb, status, lac, cellid, tech, cbd->data);
}

static void qmi_registration_status(struct ofono_netreg *netreg,
				ofono_netreg_status_cb_t cb, void *user_data)
{
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	cbd->user = data;

	if (qmi_service_send(data->nas, QMI_NAS_GET_SS_INFO, NULL,
					get_ss_info_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, -1, -1, -1, cbd->data);

	g_free(cbd);
}

static void qmi_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *user_data)
{
	struct netreg_data *data = ofono_netreg_get_data(netreg);

	DBG("");

	CALLBACK_WITH_SUCCESS(cb, &data->operator, user_data);
}

static void scan_nets_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_operator_list_cb_t cb = cbd->cb;
	struct ofono_network_operator *list;
	const struct qmi_nas_network_list *netlist;
	const struct qmi_nas_network_rat *netrat;
	const void *ptr;
	uint16_t len, num, offset, i;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
		return;
	}

	ptr = qmi_result_get(result, QMI_NAS_RESULT_NETWORK_LIST, &len);
	if (!ptr) {
		CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
		return;
	}

	netlist = ptr;

	num = GUINT16_FROM_LE(netlist->count);

	DBG("found %d operators", num);

	list = g_try_new0(struct ofono_network_operator, num);
	if (!list) {
		CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);
		return;
	}

	offset = 2;

	for (i = 0; i < num; i++) {
		const struct qmi_nas_network_info *netinfo = ptr + offset;

		snprintf(list[i].mcc, OFONO_MAX_MCC_LENGTH + 1, "%03d",
						GUINT16_FROM_LE(netinfo->mcc));
		snprintf(list[i].mnc, OFONO_MAX_MNC_LENGTH + 1, "%02d",
						GUINT16_FROM_LE(netinfo->mnc));
		strncpy(list[i].name, netinfo->desc, netinfo->desc_len);
		list[i].name[netinfo->desc_len] = '\0';

		if (netinfo->status & 0x10)
			list[i].status = 3;
		else if (netinfo->status & 0x01)
			list[i].status = 2;
		else if (netinfo->status & 0x02)
			list[i].status = 1;
		else
			list[i].status = 0;

		list[i].tech = -1;

		DBG("%s (%s:%s) status %d", list[i].name,
				list[i].mcc, list[i].mnc, list[i].status);

		offset += sizeof(struct qmi_nas_network_info) +
							netinfo->desc_len;
	}

	netrat = qmi_result_get(result, QMI_NAS_RESULT_NETWORK_RAT, &len);
	if (!netrat)
		goto done;

	if (GUINT16_FROM_LE(netrat->count) != num)
		goto done;

	for (i = 0; i < num; i++) {
		DBG("%03d:%02d %d", netrat->info[i].mcc, netrat->info[i].mnc,
							netrat->info[i].rat);

		list[i].tech = rat_to_tech(netrat->info[i].rat);
	}

done:
	CALLBACK_WITH_SUCCESS(cb, num, list, cbd->data);

	g_free(list);
}

static void qmi_list_operators(struct ofono_netreg *netreg,
			ofono_netreg_operator_list_cb_t cb, void *user_data)
{
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->nas, QMI_NAS_SCAN_NETS, NULL,
					scan_nets_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, 0, NULL, cbd->data);

	g_free(cbd);
}

static void register_net_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_register_cb_t cb = cbd->cb;
	uint16_t error;

	DBG("");

	if (qmi_result_set_error(result, &error)) {
		if (error == 26) {
			/* no effect */
			goto done;
		}

		CALLBACK_WITH_FAILURE(cb, cbd->data);
		return;
	}

done:
	CALLBACK_WITH_SUCCESS(cb, cbd->data);
}

static void qmi_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb, void *user_data)
{
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_param *param;

	DBG("");

	param = qmi_param_new_uint8(QMI_NAS_PARAM_REGISTER_ACTION,
					QMI_NAS_REGISTER_ACTION_AUTO);
	if (!param)
		goto error;

	if (qmi_service_send(data->nas, QMI_NAS_REGISTER_NET, param,
					register_net_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static void qmi_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *user_data)
{
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, user_data);
	struct qmi_nas_param_register_manual_info info;
	struct qmi_param *param;

	DBG("");

	param = qmi_param_new_uint8(QMI_NAS_PARAM_REGISTER_ACTION,
					QMI_NAS_REGISTER_ACTION_MANUAL);
	if (!param)
		goto error;

	info.mcc = atoi(mcc);
	info.mnc = atoi(mnc);
	info.rat = data->current_rat;

	qmi_param_append(param, QMI_NAS_PARAM_REGISTER_MANUAL_INFO,
						sizeof(info), &info);

	if (qmi_service_send(data->nas, QMI_NAS_REGISTER_NET, param,
					register_net_cb, cbd, g_free) > 0)
		return;

	qmi_param_free(param);

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);

	g_free(cbd);
}

static int dbm_to_strength(int8_t dbm)
{
	if (dbm > -55)
		return 100;
	else if (dbm > -65)
		return 80;
	else if (dbm > -75)
		return 60;
	else if (dbm > -85)
		return 40;
	else if (dbm > -95)
		return 20;
	else if (dbm > -105)
		return 0;

	return -1;
}

static void get_rssi_cb(struct qmi_result *result, void *user_data)
{
	struct cb_data *cbd = user_data;
	ofono_netreg_strength_cb_t cb = cbd->cb;
	const struct qmi_nas_signal_strength *ss;
	uint16_t len;
	int strength;

	DBG("");

	if (qmi_result_set_error(result, NULL)) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	ss = qmi_result_get(result, QMI_NAS_RESULT_SIGNAL_STRENGTH, &len);
	if (!ss) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	DBG("signal with %d dBm on %d", ss->dbm, ss->rat);

	strength = dbm_to_strength(ss->dbm);

	CALLBACK_WITH_SUCCESS(cb, strength, cbd->data);
}

static void qmi_signal_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *user_data)
{
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	struct cb_data *cbd = cb_data_new(cb, user_data);

	DBG("");

	if (qmi_service_send(data->nas, QMI_NAS_GET_RSSI, NULL,
					get_rssi_cb, cbd, g_free) > 0)
		return;

	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);

	g_free(cbd);
}

static void event_notify(struct qmi_result *result, void *user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	const struct qmi_nas_signal_strength *ss;
	const struct qmi_nas_rf_info *rf;
	uint16_t len;

	DBG("");

	ss = qmi_result_get(result, QMI_NAS_NOTIFY_SIGNAL_STRENGTH, &len);
        if (ss) {
		int strength;

		DBG("signal with %d dBm on %d", ss->dbm, ss->rat);

		strength = dbm_to_strength(ss->dbm);

		ofono_netreg_strength_notify(netreg, strength);
	}

	rf = qmi_result_get(result, QMI_NAS_NOTIFY_RF_INFO, &len);
	if (rf) {
		uint8_t i;

		for (i = 0; i < rf->count; i++) {
			DBG("rat %d band %d channel %d", rf->info[i].rat,
					rf->info[i].band, rf->info[i].channel);
		}

		data->current_rat = rf->info[i].rat;
	}
}

static void set_event_cb(struct qmi_result *result, void *user_data)
{
	struct ofono_netreg *netreg = user_data;

	DBG("");

	ofono_netreg_register(netreg);
}

static void create_nas_cb(struct qmi_service *service, void *user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *data = ofono_netreg_get_data(netreg);
	struct qmi_param *param;
	struct qmi_nas_param_event_signal_strength ss = { .report = 0x01,
				.count = 5, .dbm[0] = -55, .dbm[1] = -65,
				.dbm[2] = -75, .dbm[3] = -85, .dbm[4] = -95 };

	DBG("");

	if (!service) {
		ofono_error("Failed to request NAS service");
		ofono_netreg_remove(netreg);
		return;
	}

	data->nas = qmi_service_ref(service);

	qmi_service_register(data->nas, QMI_NAS_EVENT,
					event_notify, netreg, NULL);

	qmi_service_register(data->nas, QMI_NAS_SS_INFO_IND,
					ss_info_notify, netreg, NULL);

	param = qmi_param_new();
	if (!param)
		goto done;

	qmi_param_append(param, QMI_NAS_PARAM_REPORT_SIGNAL_STRENGTH,
							sizeof(ss), &ss);
	qmi_param_append_uint8(param, QMI_NAS_PARAM_REPORT_RF_INFO, 0x01);

	if (qmi_service_send(data->nas, QMI_NAS_SET_EVENT, param,
					set_event_cb, netreg, NULL) > 0)
		return;

	qmi_param_free(param);

done:
	ofono_netreg_register(netreg);
}

static int qmi_netreg_probe(struct ofono_netreg *netreg,
				unsigned int vendor, void *user_data)
{
	struct qmi_device *device = user_data;
	struct netreg_data *data;

	DBG("");

	data = g_new0(struct netreg_data, 1);

	data->operator.name[0] = '\0';
	data->operator.mcc[0] = '\0';
	data->operator.mnc[0] = '\0';
	data->operator.status = -1;
	data->operator.tech = -1;

	data->current_rat = QMI_NAS_NETWORK_RAT_NO_CHANGE;

	ofono_netreg_set_data(netreg, data);

	qmi_service_create(device, QMI_SERVICE_NAS,
					create_nas_cb, netreg, NULL);

	return 0;
}

static void qmi_netreg_remove(struct ofono_netreg *netreg)
{
	struct netreg_data *data = ofono_netreg_get_data(netreg);

	DBG("");

	ofono_netreg_set_data(netreg, NULL);

	qmi_service_unregister_all(data->nas);

	qmi_service_unref(data->nas);

	g_free(data);
}

static struct ofono_netreg_driver driver = {
	.name			= "qmimodem",
	.probe			= qmi_netreg_probe,
	.remove			= qmi_netreg_remove,
	.registration_status	= qmi_registration_status,
	.current_operator	= qmi_current_operator,
	.list_operators		= qmi_list_operators,
	.register_auto		= qmi_register_auto,
	.register_manual	= qmi_register_manual,
	.strength		= qmi_signal_strength,
};

void qmi_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void qmi_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
