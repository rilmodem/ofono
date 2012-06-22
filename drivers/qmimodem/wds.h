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

#define QMI_WDS_START_NET	32	/* Start WDS network interface */
#define QMI_WDS_STOP_NET	33	/* Stop WDS network interface */
#define QMI_WDS_GET_PKT_STATUS	34	/* Get packet data connection status */
#define QMI_WDS_PKT_STATUS_IND	34	/* Packet data connection status indication */

#define QMI_WDS_GET_SETTINGS	45	/* Get the runtime data session settings */


/* Start WDS network interface */
#define QMI_WDS_PARAM_APN			0x14	/* string */
#define QMI_WDS_PARAM_IP_FAMILY			0x19	/* uint8 */

#define QMI_WDS_RESULT_PKT_HANDLE		0x01	/* uint32 */

/* Stop WDS network interface */
#define QMI_WDS_PARAM_PKT_HANDLE		0x01	/* uint32 */

/* Packet data connection status indication */
#define QMI_WDS_NOTIFY_CONN_STATUS		0x01
struct qmi_wds_notify_conn_status {
	uint8_t status;
	uint8_t reconf;
} __attribute__((__packed__));
#define QMI_WDS_NOTIFY_IP_FAMILY		0x12	/* uint8 */

#define QMI_WDS_CONN_STATUS_DISCONNECTED	0x01
#define QMI_WDS_CONN_STATUS_CONNECTED		0x02
#define QMI_WDS_CONN_STATUS_SUSPENDED		0x03
#define QMI_WDS_CONN_STATUS_AUTHENTICATING	0x04

/* Get the runtime data session settings */
#define QMI_WDS_RESULT_PDP_TYPE			0x11	/* uint8 */
#define QMI_WDS_RESULT_PRIMARY_DNS		0x15	/* uint32 IPv4 */
#define QMI_WDS_RESULT_SECONDARY_DNS		0x16	/* uint32 IPv4 */
#define QMI_WDS_RESULT_IP_ADDRESS		0x1e	/* uint32 IPv4 */
#define QMI_WDS_RESULT_GATEWAY			0x20	/* uint32 IPv4 */
#define QMI_WDS_RESULT_IP_FAMILY		0x2b	/* uint8 */

#define QMI_WDS_PDP_TYPE_IPV4			0x00
#define QMI_WDS_PDP_TYPE_PPP			0x01
#define QMI_WDS_PDP_TYPE_IPV6			0x02
#define QMI_WDS_PDP_TYPE_IPV4V6			0x03
