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

#define QMI_UIM_READ_TRANSPARENT	32	/* Read data */
#define QMI_UIM_READ_RECORD		33	/* Read one or more records */
#define QMI_UIM_WRITE_TRANSPARENT	34	/* Write data */
#define QMI_UIM_WRITE_RECORD		35	/* Write a record */
#define QMI_UIM_GET_FILE_ATTRIBUTES	36	/* Get file attributes */

#define QMI_UIM_EVENT_REGISTRATION	46	/* Register for indications */
#define QMI_UIM_GET_CARD_STATUS		47	/* Get card status */


/* Register for indications */
#define QMI_UIM_PARAM_EVENT_MASK	0x01	/* uint32 */
#define QMI_UIM_RESULT_EVENT_MASK	0x10	/* uint32 */

#define QMI_UIM_RESULT_CARD_STATUS	0x10
struct qmi_uim_card_status {
	uint16_t index_gw_pri;
	uint16_t index_1x_pri;
	uint16_t index_gw_sec;
	uint16_t index_1x_sec;
	uint8_t num_slot;
} __attribute__((__packed__));

struct qmi_uim_slot_info {
	uint8_t card_state;
	uint8_t upin_state;
	uint8_t upin_retries;
	uint8_t upuk_retries;
	uint8_t error_code;
	uint8_t num_app;
} __attribute__((__packed__));

struct qmi_uim_app_info1 {
	uint8_t app_type;
	uint8_t app_state;
	uint8_t perso_state;
	uint8_t perso_feature;
	uint8_t perso_retries;
	uint8_t perso_unblock_retries;
	uint8_t aid_len;
	uint8_t aid_value[0];
} __attribute__((__packed__));

struct qmi_uim_app_info2 {
	uint8_t univ_pin;
	uint8_t pin1_state;
	uint8_t pin1_retries;
	uint8_t puk1_retries;
	uint8_t pin2_state;
	uint8_t pin2_retries;
	uint8_t puk2_retries;
} __attribute__((__packed__));

struct qmi_uim_file_attributes {
	uint16_t file_size;
	uint16_t file_id;
	uint8_t file_type;
	uint16_t rec_size;
	uint16_t rec_count;
	uint8_t sec_read;
	uint16_t sec_read_mask;
	uint8_t sec_write;
	uint16_t sec_write_mask;
	uint8_t sec_increase;
	uint16_t sec_increase_mask;
	uint8_t sec_deactivate;
	uint16_t sec_deactivate_mask;
	uint8_t sec_activate;
	uint16_t sec_activate_mask;
	uint16_t raw_len;
	uint8_t raw_value[0];
} __attribute__((__packed__));
