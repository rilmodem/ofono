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

#define QMI_DMS_RESET			0	/* Reset DMS service */
#define QMI_DMS_EVENT			1	/* Event report indication */
#define QMI_DMS_SET_EVENT		1	/* Set report conditions */
#define QMI_DMS_GET_CAPS		32	/* Get device capabilities */
#define QMI_DMS_GET_MANUFACTURER	33	/* Get device manfacturer */
#define QMI_DMS_GET_MODEL_ID		34	/* Get device model ID */
#define QMI_DMS_GET_REV_ID		35	/* Get device revision ID */
#define QMI_DMS_GET_NUMBER		36	/* Get assigned voice number */
#define QMI_DMS_GET_IDS			37	/* Get ESN/IMEI/MEID */
#define QMI_DMS_GET_POWER_STATE		38	/* Get power state */
#define QMI_DMS_SET_PIN_PROTECT		39	/* Set PIN protection */
#define QMI_DMS_PIN_VERIFY		40	/* Verify PIN */
#define QMI_DMS_PIN_UNBLOCK		41	/* Unblock PIN */
#define QMI_DMS_PIN_CHANGE		42	/* Change PIN */
#define QMI_DMS_GET_PIN_STATUS		43	/* Get PIN status */
#define QMI_DMS_GET_MSM_ID		44	/* Get MSM ID */
#define QMI_DMS_GET_OPER_MODE		45	/* Get operating mode */
#define QMI_DMS_SET_OPER_MODE		46	/* Set operating mode */
#define QMI_DMS_GET_TIME		47	/* Get timestamp from the device */
#define QMI_DMS_GET_PRL_VERSION		48	/* Get PRL version */

#define QMI_DMS_GET_ICCID		60	/* Get UIM ICCID */

#define QMI_DMS_GET_IMSI		67	/* Get IMSI */
#define QMI_DMS_GET_UIM_STATE		68	/* Get UIM state */
#define QMI_DMS_GET_BAND_CAPS		69	/* Get device band capabilities */
#define QMI_DMS_GET_FACTORY_ID		70	/* Get device factory ID */
#define QMI_DMS_GET_SW_VERSION		81	/* Get software version */


/* Report indication */
#define QMI_DMS_NOTIFY_PIN1_STATE		0x11
#define QMI_DMS_NOTIFY_PIN2_STATE		0x12
#define QMI_DMS_NOTIFY_OPER_MODE		0x14	/* uint8 */
#define QMI_DMS_NOTIFY_UIM_STATE		0x15	/* uint8 */

#define QMI_DMS_UIM_STATE_INIT_COMPLETE		0x00
#define QMI_DMS_UIM_STATE_INIT_FAILED		0x01
#define QMI_DMS_UIM_STATE_NOT_PRESENT		0x02
#define QMI_DMS_UIM_STATE_INVALID		0xff

#define QMI_DMS_OPER_MODE_ONLINE		0x00
#define QMI_DMS_OPER_MODE_LOW_POWER		0x01
#define QMI_DMS_OPER_MODE_FACTORY_TEST		0x02
#define QMI_DMS_OPER_MODE_OFFLINE		0x03
#define QMI_DMS_OPER_MODE_RESET			0x04
#define QMI_DMS_OPER_MODE_SHUTDOWN		0x05
#define QMI_DMS_OPER_MODE_PERSIST_LOW_POWER	0x06
#define QMI_DMS_OPER_MODE_ONLY_LOW_POWER	0x07

/* Set report conditions */
#define QMI_DMS_PARAM_REPORT_PIN_STATUS		0x12	/* bool */
#define QMI_DMS_PARAM_REPORT_OPER_MODE		0x14	/* bool */
#define QMI_DMS_PARAM_REPORT_UIM_STATE		0x15	/* bool */

/* Get device capabilities */
#define QMI_DMS_RESULT_DEVICE_CAPS		0x01
struct qmi_dms_device_caps {
	uint32_t max_tx_rate;
	uint32_t max_rx_rate;
	uint8_t data_capa;
	int8_t sim_supported;
	uint8_t radio_if_count;
	uint8_t radio_if[0];
} __attribute__ ((packed));

#define QMI_DMS_DATA_CAPA_NOT_SUPPORTED		0x00
#define QMI_DMS_DATA_CAPA_CS_ONLY		0x01
#define QMI_DMS_DATA_CAPA_PS_ONLY		0x02
#define QMI_DMS_DATA_CAPA_SIMUL_CS_PS		0x03
#define QMI_DMS_DATA_CAPA_NONSIMUL_CS_PS	0x04

#define QMI_DMS_RADIO_IF_CDMA2000_1X		0x01
#define QMI_DMS_RADIO_IF_CDMA2000_HRPD		0x02
#define QMI_DMS_RADIO_IF_GSM			0x04
#define QMI_DMS_RADIO_IF_UMTS			0x05
#define QMI_DMS_RADIO_IF_LTE			0x08

/* Get device manfacturer */
#define QMI_DMS_RESULT_MANUFACTURER		0x01

/* Get device model ID */
#define QMI_DMS_RESULT_MODEL_ID			0x01

/* Get device revision ID */
#define QMI_DMS_RESULT_REV_ID			0x01

/* Get assigned voice number */
#define QMI_DMS_RESULT_VOICE_NUMBER		0x01
#define QMI_DMS_RESULT_MOBILE_ID		0x10
#define QMI_DMS_RESULT_IMSI_NUMBER		0x11

/* Get ESN/IMEI/MEID */
#define QMI_DMS_RESULT_ESN			0x10	/* optional */
#define QMI_DMS_RESULT_IMEI			0x11	/* optional */
#define QMI_DMS_RESULT_MEID			0x12	/* optional */

/* Get PIN status */
#define QMI_DMS_RESULT_PIN1_STATUS		0x11
#define QMI_DMS_RESULT_PIN2_STATUS		0x12
struct qmi_dms_pin_status {
	uint8_t status;
	uint8_t verify_retries;
	uint8_t unblock_retries;
} __attribute__ ((packed));

#define QMI_DMS_PIN_UNINITIALIZED		0x00
#define QMI_DMS_PIN_ENABLED_UNVERIFIED		0x01
#define QMI_DMS_PIN_ENABLED_VERIFIED		0x02
#define QMI_DMS_PIN_DISABLED			0x03
#define QMI_DMS_PIN_BLOCKED			0x04
#define QMI_DMS_PIN_BLOCKED_PERMANENTLY		0x05
#define QMI_DMS_PIN_UNBLOCKED			0x06
#define QMI_DMS_PIN_CHANGED			0x07

/* Get operating mode */
#define QMI_DMS_RESULT_OPER_MODE		0x01	/* uint8 */

/* Set operating mode */
#define QMI_DMS_PARAM_OPER_MODE			0x01	/* uint8 */

/* Get UIM ICCID */
#define QMI_DMS_RESULT_ICCID			0x01	/* string */

/* Get IMSI */
#define QMI_DMS_RESULT_IMSI			0x01	/* string */

/* Get UIM state */
#define QMI_DMS_RESULT_UIM_STATE		0x01	/* uint8 */

/* Get device band capabilities */
#define QMI_DMS_RESULT_BAND_CAPS		0x01	/* uint64 bitmask */
#define QMI_DMS_RESULT_LTE_BAND_CAPS		0x10	/* uint64 bitmask */
