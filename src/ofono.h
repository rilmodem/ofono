/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE

#include <ofono/types.h>

void __ofono_exit(void);

int __ofono_manager_init(void);
void __ofono_manager_cleanup(void);

int __ofono_handsfree_audio_manager_init(void);
void __ofono_handsfree_audio_manager_cleanup(void);

void __ofono_modem_shutdown(void);

#include <ofono/log.h>

int __ofono_log_init(const char *program, const char *debug,
						ofono_bool_t detach);
void __ofono_log_cleanup(void);
void __ofono_log_enable(struct ofono_debug_desc *start,
					struct ofono_debug_desc *stop);

#include <ofono/dbus.h>

int __ofono_dbus_init(DBusConnection *conn);
void __ofono_dbus_cleanup(void);

DBusMessage *__ofono_error_invalid_args(DBusMessage *msg);
DBusMessage *__ofono_error_invalid_format(DBusMessage *msg);
DBusMessage *__ofono_error_not_implemented(DBusMessage *msg);
DBusMessage *__ofono_error_failed(DBusMessage *msg);
DBusMessage *__ofono_error_busy(DBusMessage *msg);
DBusMessage *__ofono_error_not_found(DBusMessage *msg);
DBusMessage *__ofono_error_not_active(DBusMessage *msg);
DBusMessage *__ofono_error_not_supported(DBusMessage *msg);
DBusMessage *__ofono_error_not_available(DBusMessage *msg);
DBusMessage *__ofono_error_timed_out(DBusMessage *msg);
DBusMessage *__ofono_error_sim_not_ready(DBusMessage *msg);
DBusMessage *__ofono_error_in_use(DBusMessage *msg);
DBusMessage *__ofono_error_not_attached(DBusMessage *msg);
DBusMessage *__ofono_error_attach_in_progress(DBusMessage *msg);
DBusMessage *__ofono_error_not_registered(DBusMessage *msg);
DBusMessage *__ofono_error_canceled(DBusMessage *msg);
DBusMessage *__ofono_error_access_denied(DBusMessage *msg);
DBusMessage *__ofono_error_emergency_active(DBusMessage *msg);
DBusMessage *__ofono_error_incorrect_password(DBusMessage *msg);
DBusMessage *__ofono_error_not_allowed(DBusMessage *msg);
DBusMessage *__ofono_error_not_recognized(DBusMessage *msg);
DBusMessage *__ofono_error_network_terminated(DBusMessage *msg);

DBusMessage *__ofono_error_from_error(const struct ofono_error *error,
						DBusMessage *msg);

void __ofono_dbus_pending_reply(DBusMessage **msg, DBusMessage *reply);

gboolean __ofono_dbus_valid_object_path(const char *path);

struct ofono_watchlist_item {
	unsigned int id;
	void *notify;
	void *notify_data;
	ofono_destroy_func destroy;
};

struct ofono_watchlist {
	int next_id;
	GSList *items;
	ofono_destroy_func destroy;
};

struct ofono_watchlist *__ofono_watchlist_new(ofono_destroy_func destroy);
unsigned int __ofono_watchlist_add_item(struct ofono_watchlist *watchlist,
					struct ofono_watchlist_item *item);
gboolean __ofono_watchlist_remove_item(struct ofono_watchlist *watchlist,
					unsigned int id);
void __ofono_watchlist_free(struct ofono_watchlist *watchlist);

#include <ofono/plugin.h>

int __ofono_plugin_init(const char *pattern, const char *exclude);
void __ofono_plugin_cleanup(void);

#include <ofono/modem.h>

typedef void (*ofono_modem_foreach_func)(struct ofono_modem *modem,
						void *data);
void __ofono_modem_foreach(ofono_modem_foreach_func cb, void *userdata);

unsigned int __ofono_modem_callid_next(struct ofono_modem *modem);
void __ofono_modem_callid_hold(struct ofono_modem *modem, int id);
void __ofono_modem_callid_release(struct ofono_modem *modem, int id);
void __ofono_modem_append_properties(struct ofono_modem *modem,
						DBusMessageIter *dict);

struct ofono_atom;

enum ofono_atom_type {
	OFONO_ATOM_TYPE_DEVINFO,
	OFONO_ATOM_TYPE_CALL_BARRING,
	OFONO_ATOM_TYPE_CALL_FORWARDING,
	OFONO_ATOM_TYPE_CALL_METER,
	OFONO_ATOM_TYPE_CALL_SETTINGS,
	OFONO_ATOM_TYPE_NETREG,
	OFONO_ATOM_TYPE_PHONEBOOK,
	OFONO_ATOM_TYPE_SMS,
	OFONO_ATOM_TYPE_SIM,
	OFONO_ATOM_TYPE_USSD,
	OFONO_ATOM_TYPE_VOICECALL,
	OFONO_ATOM_TYPE_HISTORY,
	OFONO_ATOM_TYPE_SSN,
	OFONO_ATOM_TYPE_MESSAGE_WAITING,
	OFONO_ATOM_TYPE_CBS,
	OFONO_ATOM_TYPES_CALL_VOLUME,
	OFONO_ATOM_TYPE_GPRS,
	OFONO_ATOM_TYPE_GPRS_CONTEXT,
	OFONO_ATOM_TYPE_RADIO_SETTINGS,
	OFONO_ATOM_TYPE_AUDIO_SETTINGS,
	OFONO_ATOM_TYPE_STK,
	OFONO_ATOM_TYPE_NETTIME,
	OFONO_ATOM_TYPE_CTM,
	OFONO_ATOM_TYPE_CDMA_VOICECALL_MANAGER,
	OFONO_ATOM_TYPE_CDMA_CONNMAN,
	OFONO_ATOM_TYPE_SIM_AUTH,
	OFONO_ATOM_TYPE_EMULATOR_DUN,
	OFONO_ATOM_TYPE_EMULATOR_HFP,
	OFONO_ATOM_TYPE_LOCATION_REPORTING,
	OFONO_ATOM_TYPE_GNSS,
	OFONO_ATOM_TYPE_CDMA_SMS,
	OFONO_ATOM_TYPE_CDMA_NETREG,
	OFONO_ATOM_TYPE_HANDSFREE,
	OFONO_ATOM_TYPE_SIRI,
};

enum ofono_atom_watch_condition {
	OFONO_ATOM_WATCH_CONDITION_REGISTERED,
	OFONO_ATOM_WATCH_CONDITION_UNREGISTERED
};

typedef void (*ofono_atom_watch_func)(struct ofono_atom *atom,
					enum ofono_atom_watch_condition cond,
					void *data);

typedef void (*ofono_atom_func)(struct ofono_atom *atom, void *data);

struct ofono_atom *__ofono_modem_add_atom(struct ofono_modem *modem,
					enum ofono_atom_type type,
					void (*destruct)(struct ofono_atom *),
					void *data);

struct ofono_atom *__ofono_modem_add_atom_offline(struct ofono_modem *modem,
					enum ofono_atom_type type,
					void (*destruct)(struct ofono_atom *),
					void *data);

struct ofono_atom *__ofono_modem_find_atom(struct ofono_modem *modem,
						enum ofono_atom_type type);

void __ofono_modem_foreach_atom(struct ofono_modem *modem,
				enum ofono_atom_type type,
				ofono_atom_func callback, void *data);

void __ofono_modem_foreach_registered_atom(struct ofono_modem *modem,
						enum ofono_atom_type type,
						ofono_atom_func callback,
						void *data);

void *__ofono_atom_get_data(struct ofono_atom *atom);
const char *__ofono_atom_get_path(struct ofono_atom *atom);
struct ofono_modem *__ofono_atom_get_modem(struct ofono_atom *atom);

#define __ofono_atom_find(enum_type, modem)			\
({								\
	struct ofono_atom *tmp_atom =				\
		__ofono_modem_find_atom(modem, enum_type);	\
								\
	tmp_atom ? __ofono_atom_get_data(tmp_atom) : NULL;	\
})

void __ofono_atom_register(struct ofono_atom *atom,
				void (*unregister)(struct ofono_atom *));
void __ofono_atom_unregister(struct ofono_atom *atom);

gboolean __ofono_atom_get_registered(struct ofono_atom *atom);

unsigned int __ofono_modem_add_atom_watch(struct ofono_modem *modem,
					enum ofono_atom_type type,
					ofono_atom_watch_func notify,
					void *data,
					ofono_destroy_func destroy);
gboolean __ofono_modem_remove_atom_watch(struct ofono_modem *modem,
						unsigned int id);

void __ofono_atom_free(struct ofono_atom *atom);

typedef void (*ofono_modemwatch_cb_t)(struct ofono_modem *modem,
					gboolean added, void *data);
void __ofono_modemwatch_init(void);
void __ofono_modemwatch_cleanup(void);
unsigned int __ofono_modemwatch_add(ofono_modemwatch_cb_t cb, void *user,
					ofono_destroy_func destroy);
gboolean __ofono_modemwatch_remove(unsigned int id);

typedef void (*ofono_modem_online_notify_func)(struct ofono_modem *modem,
						ofono_bool_t online,
						void *data);
unsigned int __ofono_modem_add_online_watch(struct ofono_modem *modem,
					ofono_modem_online_notify_func notify,
					void *data, ofono_destroy_func destroy);
void __ofono_modem_remove_online_watch(struct ofono_modem *modem,
					unsigned int id);

typedef void (*ofono_modem_powered_notify_func)(struct ofono_modem *modem,
						ofono_bool_t powered,
						void *data);

unsigned int __ofono_modem_add_powered_watch(struct ofono_modem *modem,
					ofono_modem_online_notify_func notify,
					void *data, ofono_destroy_func destroy);
void __ofono_modem_remove_powered_watch(struct ofono_modem *modem,
					unsigned int id);

void __ofono_modem_sim_reset(struct ofono_modem *modem);

void __ofono_modem_inc_emergency_mode(struct ofono_modem *modem);
void __ofono_modem_dec_emergency_mode(struct ofono_modem *modem);

#include <ofono/call-barring.h>

gboolean __ofono_call_barring_is_busy(struct ofono_call_barring *cb);

#include <ofono/call-forwarding.h>

gboolean __ofono_call_forwarding_is_busy(struct ofono_call_forwarding *cf);

#include <ofono/call-meter.h>
#include <ofono/call-settings.h>

gboolean __ofono_call_settings_is_busy(struct ofono_call_settings *cs);

#include <ofono/cbs.h>
#include <ofono/devinfo.h>
#include <ofono/phonebook.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <ofono/radio-settings.h>
#include <ofono/audio-settings.h>
#include <ofono/ctm.h>
#include <ofono/location-reporting.h>

#include <ofono/voicecall.h>

enum ofono_voicecall_interaction {
	OFONO_VOICECALL_INTERACTION_NONE	= 0,
	OFONO_VOICECALL_INTERACTION_PUT_ON_HOLD	= 1,
	OFONO_VOICECALL_INTERACTION_DISCONNECT	= 2,
};

typedef void (*ofono_voicecall_dial_cb_t)(struct ofono_call *call, void *data);
typedef void (*ofono_voicecall_tone_cb_t)(int error, void *data);

ofono_bool_t __ofono_voicecall_is_busy(struct ofono_voicecall *vc,
					enum ofono_voicecall_interaction type);

int __ofono_voicecall_dial(struct ofono_voicecall *vc,
				const char *addr, int addr_type,
				const char *message, unsigned char icon_id,
				enum ofono_voicecall_interaction interaction,
				ofono_voicecall_dial_cb_t cb, void *user_data);
void __ofono_voicecall_dial_cancel(struct ofono_voicecall *vc);

void __ofono_voicecall_set_alpha_and_icon_id(struct ofono_voicecall *vc,
						const char *addr, int addr_type,
						const char *message,
						unsigned char icon_id);
void __ofono_voicecall_clear_alpha_and_icon_id(struct ofono_voicecall *vc);

int __ofono_voicecall_tone_send(struct ofono_voicecall *vc,
				const char *tone_str,
				ofono_voicecall_tone_cb_t cb, void *user_data);
void __ofono_voicecall_tone_cancel(struct ofono_voicecall *vc, int id);

struct ofono_call *__ofono_voicecall_find_call_with_status(
				struct ofono_voicecall *vc, int status);

#include <ofono/sms.h>

struct sms;

enum ofono_sms_submit_flag {
	OFONO_SMS_SUBMIT_FLAG_REQUEST_SR =	0x1,
	OFONO_SMS_SUBMIT_FLAG_RECORD_HISTORY =	0x2,
	OFONO_SMS_SUBMIT_FLAG_RETRY =		0x4,
	OFONO_SMS_SUBMIT_FLAG_EXPOSE_DBUS =	0x8,
	OFONO_SMS_SUBMIT_FLAG_REUSE_UUID =	0x10,
};

typedef void (*ofono_sms_txq_submit_cb_t)(gboolean ok, void *data);
typedef void (*ofono_sms_txq_queued_cb_t)(struct ofono_sms *sms,
						const struct ofono_uuid *uuid,
						void *data);
typedef void (*ofono_sms_text_notify_cb_t)(const char *from,
						const struct tm *remote,
						const struct tm *local,
						const char *text,
						void *data);
typedef void (*ofono_sms_datagram_notify_cb_t)(const char *from,
						const struct tm *remote,
						const struct tm *local,
						int dst, int src,
						const unsigned char *buffer,
						unsigned int len,
						void *data);

int __ofono_sms_txq_submit(struct ofono_sms *sms, GSList *list,
				unsigned int flags, struct ofono_uuid *uuid,
				ofono_sms_txq_queued_cb_t, void *data);

int __ofono_sms_txq_set_submit_notify(struct ofono_sms *sms,
					struct ofono_uuid *uuid,
					ofono_sms_txq_submit_cb_t cb,
					void *data,
					ofono_destroy_func destroy);

int __ofono_sms_txq_cancel(struct ofono_sms *sms,
				const struct ofono_uuid *uuid);

const char *__ofono_sms_message_path_from_uuid(struct ofono_sms *sms,
						const struct ofono_uuid *uuid);

unsigned int __ofono_sms_text_watch_add(struct ofono_sms *sms,
					ofono_sms_text_notify_cb_t cb,
					void *data, ofono_destroy_func destroy);
gboolean __ofono_sms_text_watch_remove(struct ofono_sms *sms,
					unsigned int id);

unsigned int __ofono_sms_datagram_watch_add(struct ofono_sms *sms,
					ofono_sms_datagram_notify_cb_t cb,
					int dst, int src, void *data,
					ofono_destroy_func destroy);
gboolean __ofono_sms_datagram_watch_remove(struct ofono_sms *sms,
					unsigned int id);

unsigned short __ofono_sms_get_next_ref(struct ofono_sms *sms);

#include <ofono/sim.h>

ofono_bool_t __ofono_sim_service_available(struct ofono_sim *sim,
						int ust_service,
						int sst_service);
ofono_bool_t __ofono_sim_cphs_service_available(struct ofono_sim *sim,
						int cphs_service);

ofono_bool_t __ofono_is_valid_sim_pin(const char *pin,
					enum ofono_sim_password_type type);

ofono_bool_t __ofono_is_valid_net_pin(const char *pin);

void __ofono_sim_refresh(struct ofono_sim *sim, GSList *file_list,
				ofono_bool_t full_file_change,
				ofono_bool_t naa_init);

void __ofono_sim_recheck_pin(struct ofono_sim *sim);

#include <ofono/stk.h>

typedef void (*__ofono_sms_sim_download_cb_t)(ofono_bool_t ok,
						const unsigned char *tp_ud,
						int len, void *data);

struct cbs;
void __ofono_cbs_sim_download(struct ofono_stk *stk, const struct cbs *msg);

struct sms;
int __ofono_sms_sim_download(struct ofono_stk *stk, const struct sms *msg,
				__ofono_sms_sim_download_cb_t cb, void *data);

#include <ofono/ussd.h>

typedef gboolean (*ofono_ussd_ssc_cb_t)(int type,
					const char *sc,
					const char *sia, const char *sib,
					const char *sic, const char *dn,
					DBusMessage *msg, void *data);

typedef gboolean (*ofono_ussd_passwd_cb_t)(const char *sc,
					const char *old, const char *new,
					DBusMessage *msg, void *data);

typedef void (*ofono_ussd_request_cb_t)(int error, int dcs,
					const unsigned char *pdu, int len,
					void *data);

gboolean __ofono_ussd_ssc_register(struct ofono_ussd *ussd, const char *sc,
					ofono_ussd_ssc_cb_t cb, void *data,
					ofono_destroy_func destroy);
void __ofono_ussd_ssc_unregister(struct ofono_ussd *ussd, const char *sc);

gboolean __ofono_ussd_passwd_register(struct ofono_ussd *ussd, const char *sc,
					ofono_ussd_passwd_cb_t cb, void *data,
					ofono_destroy_func destroy);
void __ofono_ussd_passwd_unregister(struct ofono_ussd *ussd, const char *sc);
gboolean __ofono_ussd_is_busy(struct ofono_ussd *ussd);

int __ofono_ussd_initiate(struct ofono_ussd *ussd, int dcs,
			const unsigned char *pdu, int len,
			ofono_ussd_request_cb_t cb, void *user_data);
void __ofono_ussd_initiate_cancel(struct ofono_ussd *ussd);

#include <ofono/netreg.h>

typedef void (*ofono_netreg_status_notify_cb_t)(int status, int lac, int ci,
			int tech, const char *mcc, const char *mnc,
			void *data);

unsigned int __ofono_netreg_add_status_watch(struct ofono_netreg *netreg,
				ofono_netreg_status_notify_cb_t cb,
				void *data, ofono_destroy_func destroy);

gboolean __ofono_netreg_remove_status_watch(struct ofono_netreg *netreg,
						unsigned int id);

void __ofono_netreg_set_base_station_name(struct ofono_netreg *netreg,
						const char *name);

#include <ofono/history.h>

void __ofono_history_probe_drivers(struct ofono_modem *modem);

void __ofono_history_call_ended(struct ofono_modem *modem,
				const struct ofono_call *call,
				time_t start, time_t end);

void __ofono_history_call_missed(struct ofono_modem *modem,
				const struct ofono_call *call, time_t when);

void __ofono_history_sms_received(struct ofono_modem *modem,
					const struct ofono_uuid *uuid,
					const char *from,
					const struct tm *remote,
					const struct tm *local,
					const char *text);

void __ofono_history_sms_send_pending(struct ofono_modem *modem,
					const struct ofono_uuid *uuid,
					const char *to,
					time_t when, const char *text);

void __ofono_history_sms_send_status(struct ofono_modem *modem,
					const struct ofono_uuid *uuid,
					time_t when,
					enum ofono_history_sms_status status);

#include <ofono/message-waiting.h>

struct sms;

void __ofono_message_waiting_mwi(struct ofono_message_waiting *mw,
				struct sms *sms, gboolean *out_discard);

const struct ofono_phone_number *__ofono_message_waiting_get_mbdn(
					struct ofono_message_waiting *mw,
					unsigned int index);

#include <ofono/nettime.h>

void __ofono_nettime_probe_drivers(struct ofono_modem *modem);

void __ofono_nettime_info_received(struct ofono_modem *modem,
					struct ofono_network_time *info);

#include <ofono/cdma-voicecall.h>
#include <ofono/cdma-connman.h>
#include <ofono/sim-auth.h>

#include <ofono/gprs-provision.h>
ofono_bool_t __ofono_gprs_provision_get_settings(const char *mcc,
				const char *mnc, const char *spn,
				struct ofono_gprs_provision_data **settings,
				int *count);
void __ofono_gprs_provision_free_settings(
				struct ofono_gprs_provision_data *settings,
				int count);

#include <ofono/emulator.h>

enum ofono_emulator_slc_condition {
	OFONO_EMULATOR_SLC_CONDITION_CMER,
	OFONO_EMULATOR_SLC_CONDITION_CHLD,
	OFONO_EMULATOR_SLC_CONDITION_BIND,
};

void __ofono_emulator_set_indicator_forced(struct ofono_emulator *em,
						const char *name, int value);
void __ofono_emulator_slc_condition(struct ofono_emulator *em,
					enum ofono_emulator_slc_condition cond);

#include <ofono/gnss.h>
#include <ofono/cdma-sms.h>
#include <ofono/cdma-netreg.h>

#include <ofono/cdma-provision.h>
ofono_bool_t __ofono_cdma_provision_get_name(const char *sid, char **name);

#include <ofono/private-network.h>

void __ofono_private_network_release(int id);
ofono_bool_t __ofono_private_network_request(ofono_private_network_cb_t cb,
						int *id, void *data);
