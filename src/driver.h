/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include <ofono/types.h>

struct ofono_modem;

enum ofono_disconnect_reason {
	OFONO_DISCONNECT_REASON_UNKNOWN = 0,
	OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
	OFONO_DISCONNECT_REASON_REMOTE_HANGUP,
	OFONO_DISCONNECT_REASON_ERROR,
};

struct ofono_call {
	unsigned id;
	int type;
	int direction;
	int status;
	struct ofono_phone_number phone_number;
	int clip_validity;
};

/* Theoretical limit is 16, but each GSM char can be encoded into
 *  * 3 UTF8 characters resulting in 16*3=48 chars
 *   */
#define OFONO_MAX_OPERATOR_NAME_LENGTH 63

struct ofono_network_operator {
	char name[OFONO_MAX_OPERATOR_NAME_LENGTH + 1];
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int status;
	int tech;
};

/* Notification functions, the integer values here should map to
 * values obtained from the modem.  The enumerations are the same
 * as the values for the fields found in 3GPP TS 27.007
 *
 * Pass in the integer value -1 if the value is not known
 * Pass in NULL string value if the value is not known
 */
typedef void (*ofono_generic_cb_t)(const struct ofono_error *error,
					void *data);

typedef void (*ofono_call_list_cb_t)(const struct ofono_error *error,
					int numcalls,
					const struct ofono_call *call_list,
					void *data);

typedef void (*ofono_current_operator_cb_t)(const struct ofono_error *error,
					const struct ofono_network_operator *op,
					void *data);

typedef void (*ofono_operator_list_cb_t)(const struct ofono_error *error,
					int total,
					const struct ofono_network_operator *list,
					void *data);

typedef void (*ofono_registration_status_cb_t)(const struct ofono_error *error,
					int status, int lac, int ci, int tech,
					void *data);

typedef void (*ofono_signal_strength_cb_t)(const struct ofono_error *error,
					int strength, void *data);

typedef void (*ofono_modem_attribute_query_cb_t)(const struct ofono_error *error,
					const char *attribute, void *data);

struct ofono_modem_attribute_ops {
	void (*query_manufacturer)(struct ofono_modem *modem,
			ofono_modem_attribute_query_cb_t cb, void *data);
	void (*query_serial)(struct ofono_modem *modem,
			ofono_modem_attribute_query_cb_t cb, void *data);
	void (*query_model)(struct ofono_modem *modem,
			ofono_modem_attribute_query_cb_t cb, void *data);
	void (*query_revision)(struct ofono_modem *modem,
			ofono_modem_attribute_query_cb_t cb, void *data);
};

struct ofono_modem *ofono_modem_register(struct ofono_modem_attribute_ops *ops);
int ofono_modem_unregister(struct ofono_modem *modem);

/* Network related functions, including registration status, operator selection
 * and signal strength indicators.
 *
 * It is up to the plugin to implement CSQ polling if the modem does not support
 * vendor extensions for signal strength notification.
 */
struct ofono_network_registration_ops {
	void (*registration_status)(struct ofono_modem *modem,
			ofono_registration_status_cb_t cb, void *data);
	void (*current_operator)(struct ofono_modem *modem,
			ofono_current_operator_cb_t cb, void *data);
	void (*list_operators)(struct ofono_modem *modem,
			ofono_operator_list_cb_t cb, void *data);
	void (*register_auto)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*register_manual)(struct ofono_modem *modem,
			const struct ofono_network_operator *oper,
			ofono_generic_cb_t cb, void *data);
	void (*deregister)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*signal_strength)(struct ofono_modem *modem,
			ofono_signal_strength_cb_t, void *data);
};

void ofono_signal_strength_notify(struct ofono_modem *modem, int strength);
void ofono_network_registration_notify(struct ofono_modem *modem, int status,
			int lac, int ci, int tech);
int ofono_network_registration_register(struct ofono_modem *modem,
				struct ofono_network_registration_ops *ops);
void ofono_network_registration_unregister(struct ofono_modem *modem);

/* Voice call related functionality, including ATD, ATA, +CHLD, CTFR, CLCC
 * and VTS.
 *
 * It is up to the plugin to implement polling of CLCC if the modem does
 * not support vendor extensions for call progress indication.
 */
struct ofono_voicecall_ops {
	void (*dial)(struct ofono_modem *modem,
			const struct ofono_phone_number *number,
			enum ofono_clir_option clir, enum ofono_cug_option cug,
			ofono_generic_cb_t cb, void *data);
	void (*answer)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*hangup)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*list_calls)(struct ofono_modem *modem,
			ofono_call_list_cb_t cb, void *data);
	void (*hold_all_active)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*release_all_held)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*set_udub)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*release_all_active)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*release_specific)(struct ofono_modem *modem, int id,
			ofono_generic_cb_t cb, void *data);
	void (*private_chat)(struct ofono_modem *modem, int id,
			ofono_generic_cb_t cb, void *data);
	void (*create_multiparty)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*transfer)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*deflect)(struct ofono_modem *modem,
			const struct ofono_phone_number *ph,
			ofono_generic_cb_t cb, void *data);
	void (*swap_without_accept)(struct ofono_modem *modem,
			ofono_generic_cb_t cb, void *data);
	void (*send_tones)(struct ofono_modem *modem, const char *tones,
			ofono_generic_cb_t cb, void *data);
};

void ofono_voicecall_notify(struct ofono_modem *modem, const struct ofono_call *call);
void ofono_voicecall_disconnected(struct ofono_modem *modem, int id,
				enum ofono_disconnect_reason reason,
				const struct ofono_error *error);

int ofono_voicecall_register(struct ofono_modem *modem, struct ofono_voicecall_ops *ops);
void ofono_voicecall_unregister(struct ofono_modem *modem);

