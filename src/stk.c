/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#define _GNU_SOURCE
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <glib.h>
#include <gdbus.h>
#include <errno.h>
#include <time.h>

#include "ofono.h"

#include "common.h"
#include "smsutil.h"
#include "stkutil.h"

static GSList *g_drivers = NULL;

struct stk_timer {
	time_t expiry;
	time_t start;
};

struct ofono_stk {
	const struct ofono_stk_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	struct stk_command *pending_cmd;
	void (*cancel_cmd)(struct ofono_stk *stk);
	GQueue *envelope_q;

	struct stk_timer timers[8];
	guint timers_source;

	struct sms_submit_req *sms_submit_req;
	char *idle_mode_text;
};

struct envelope_op {
	uint8_t tlv[256];
	unsigned int tlv_len;
	int retries;
	void (*cb)(struct ofono_stk *stk, gboolean ok,
			const unsigned char *data, int length);
};

struct sms_submit_req {
	struct ofono_stk *stk;
	gboolean cancelled;
};

#define ENVELOPE_RETRIES_DEFAULT 5

static void envelope_queue_run(struct ofono_stk *stk);
static void timers_update(struct ofono_stk *stk);

static int stk_respond(struct ofono_stk *stk, struct stk_response *rsp,
			ofono_stk_generic_cb_t cb)
{
	const guint8 *tlv;
	unsigned int tlv_len;

	if (stk->driver->terminal_response == NULL)
		return -ENOSYS;

	rsp->src = STK_DEVICE_IDENTITY_TYPE_TERMINAL;
	rsp->dst = STK_DEVICE_IDENTITY_TYPE_UICC;
	rsp->number = stk->pending_cmd->number;
	rsp->type = stk->pending_cmd->type;
	rsp->qualifier = stk->pending_cmd->qualifier;

	tlv = stk_pdu_from_response(rsp, &tlv_len);
	if (!tlv)
		return -EINVAL;

	stk_command_free(stk->pending_cmd);
	stk->pending_cmd = NULL;

	stk->driver->terminal_response(stk, tlv_len, tlv, cb, stk);

	return 0;
}

static void envelope_cb(const struct ofono_error *error, const uint8_t *data,
			int length, void *user_data)
{
	struct ofono_stk *stk = user_data;
	struct envelope_op *op = g_queue_peek_head(stk->envelope_q);
	gboolean result = TRUE;

	if (op->retries > 0 && error->type == OFONO_ERROR_TYPE_SIM &&
			error->error == 0x9300) {
		op->retries--;
		goto out;
	}

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		result = FALSE;

	g_queue_pop_head(stk->envelope_q);

	if (op->cb)
		op->cb(stk, result, data, length);

	g_free(op);

out:
	envelope_queue_run(stk);
}

static void envelope_queue_run(struct ofono_stk *stk)
{
	if (g_queue_get_length(stk->envelope_q) > 0) {
		struct envelope_op *op = g_queue_peek_head(stk->envelope_q);

		stk->driver->envelope(stk, op->tlv_len, op->tlv,
					envelope_cb, stk);
	}
}

static int stk_send_envelope(struct ofono_stk *stk, struct stk_envelope *e,
				void (*cb)(struct ofono_stk *stk, gboolean ok,
						const uint8_t *data,
						int length), int retries)
{
	const uint8_t *tlv;
	unsigned int tlv_len;
	struct envelope_op *op;

	if (stk->driver->envelope == NULL)
		return -ENOSYS;

	e->dst = STK_DEVICE_IDENTITY_TYPE_UICC;
	tlv = stk_pdu_from_envelope(e, &tlv_len);
	if (!tlv)
		return -EINVAL;

	op = g_new0(struct envelope_op, 1);

	op->cb = cb;
	op->retries = retries;
	memcpy(op->tlv, tlv, tlv_len);
	op->tlv_len = tlv_len;

	g_queue_push_tail(stk->envelope_q, op);

	if (g_queue_get_length(stk->envelope_q) == 1)
		envelope_queue_run(stk);

	return 0;
}

static void stk_cbs_download_cb(struct ofono_stk *stk, gboolean ok,
				const unsigned char *data, int len)
{
	if (!ok) {
		ofono_error("CellBroadcast download to UICC failed");
		return;
	}

	if (len)
		ofono_error("CellBroadcast download returned %i bytes of data",
				len);

	DBG("CellBroadcast download to UICC reported no error");
}

void __ofono_cbs_sim_download(struct ofono_stk *stk, const struct cbs *msg)
{
	struct stk_envelope e;
	int err;

	memset(&e, 0, sizeof(e));

	e.type = STK_ENVELOPE_TYPE_CBS_PP_DOWNLOAD;
	e.src = STK_DEVICE_IDENTITY_TYPE_NETWORK;
	memcpy(&e.cbs_pp_download.page, msg, sizeof(msg));

	err = stk_send_envelope(stk, &e, stk_cbs_download_cb,
				ENVELOPE_RETRIES_DEFAULT);
	if (err)
		stk_cbs_download_cb(stk, FALSE, NULL, -1);
}

static void stk_command_cb(const struct ofono_error *error, void *data)
{
	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("TERMINAL RESPONSE to a UICC command failed");
		return;
	}

	DBG("TERMINAL RESPONSE to a command reported no errors");
}

static void stk_alpha_id_set(struct ofono_stk *stk, const char *text)
{
	/* TODO */
}

static void stk_alpha_id_unset(struct ofono_stk *stk)
{
	/* TODO */
}

static DBusMessage *stk_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_stk *stk = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	const char *idle_mode_text;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	idle_mode_text = stk->idle_mode_text ? stk->idle_mode_text : "";
	ofono_dbus_dict_append(&dict, "IdleModeText",
				DBUS_TYPE_STRING, &idle_mode_text);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable stk_methods[] = {
	{ "GetProperties",		"",	"a{sv}",stk_get_properties },

	{ }
};

static GDBusSignalTable stk_signals[] = {
	{ "PropertyChanged",	"sv" },

	{ }
};

static gboolean handle_command_more_time(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	/* Do nothing */

	return TRUE;
}

static void send_sms_cancel(struct ofono_stk *stk)
{
	stk->sms_submit_req->cancelled = TRUE;

	if (!stk->pending_cmd->send_sms.alpha_id ||
			!stk->pending_cmd->send_sms.alpha_id[0])
		return;

	stk_alpha_id_unset(stk);
}

static void send_sms_submit_cb(gboolean ok, void *data)
{
	struct sms_submit_req *req = data;
	struct ofono_stk *stk = req->stk;
	struct ofono_error failure = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_response rsp;

	ofono_debug("SMS submission %s", ok ? "successful" : "failed");

	if (req->cancelled) {
		ofono_debug("Received an SMS submitted callback after the "
				"proactive command was cancelled");
		return;
	}

	memset(&rsp, 0, sizeof(rsp));

	if (ok == FALSE)
		rsp.result.type = STK_RESULT_TYPE_NETWORK_UNAVAILABLE;

	if (stk_respond(stk, &rsp, stk_command_cb))
		stk_command_cb(&failure, stk);

	if (stk->pending_cmd->send_sms.alpha_id &&
			stk->pending_cmd->send_sms.alpha_id[0])
		stk_alpha_id_unset(stk);
}

static gboolean handle_command_send_sms(const struct stk_command *cmd,
					struct stk_response *rsp,
					struct ofono_stk *stk)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	struct ofono_atom *sms_atom;
	struct ofono_sms *sms;
	GSList msg_list;

	sms_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_SMS);

	if (!sms_atom || !__ofono_atom_get_registered(sms_atom)) {
		rsp->result.type = STK_RESULT_TYPE_NOT_CAPABLE;
		return TRUE;
	}

	sms = __ofono_atom_get_data(sms_atom);

	stk->sms_submit_req = g_new0(struct sms_submit_req, 1);
	stk->sms_submit_req->stk = stk;

	msg_list.data = (void *) &cmd->send_sms.gsm_sms;
	msg_list.next = NULL;

	__ofono_sms_txq_submit(sms, &msg_list, 0, send_sms_submit_cb,
				stk->sms_submit_req, g_free);

	stk->cancel_cmd = send_sms_cancel;

	if (cmd->send_sms.alpha_id && cmd->send_sms.alpha_id[0])
		stk_alpha_id_set(stk, cmd->send_sms.alpha_id);

	return FALSE;
}

static gboolean handle_command_set_idle_text(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(stk->atom);
	const char *idle_mode_text;

	if (stk->idle_mode_text) {
		g_free(stk->idle_mode_text);
		stk->idle_mode_text = NULL;
	}

	if (cmd->setup_idle_mode_text.text)
		stk->idle_mode_text = g_strdup(cmd->setup_idle_mode_text.text);

	idle_mode_text = stk->idle_mode_text ? stk->idle_mode_text : "";
	ofono_dbus_signal_property_changed(conn, path, OFONO_STK_INTERFACE,
						"IdleModeText",
						DBUS_TYPE_STRING,
						&idle_mode_text);

	return TRUE;
}

static void timer_expiration_cb(struct ofono_stk *stk, gboolean ok,
				const unsigned char *data, int len)
{
	if (!ok) {
		ofono_error("Timer Expiration reporting failed");
		return;
	}

	if (len)
		ofono_error("Timer Expiration returned %i bytes of data",
				len);

	DBG("Timer Expiration reporting to UICC reported no error");
}

static gboolean timers_cb(gpointer user_data)
{
	struct ofono_stk *stk = user_data;

	stk->timers_source = 0;

	timers_update(stk);

	return FALSE;
}

static void timer_value_from_seconds(struct stk_timer_value *val, int seconds)
{
	val->has_value = TRUE;
	val->hour = seconds / 3600;
	seconds -= val->hour * 3600;
	val->minute = seconds / 60;
	seconds -= val->minute * 60;
	val->second = seconds;
}

static void timers_update(struct ofono_stk *stk)
{
	time_t min = 0, now = time(NULL);
	int i;

	if (stk->timers_source) {
		g_source_remove(stk->timers_source);
		stk->timers_source = 0;
	}

	for (i = 0; i < 8; i++) {
		if (!stk->timers[i].expiry)
			continue;

		if (stk->timers[i].expiry <= now) {
			struct stk_envelope e;
			int seconds = now - stk->timers[i].start;

			stk->timers[i].expiry = 0;

			memset(&e, 0, sizeof(e));

			e.type = STK_ENVELOPE_TYPE_TIMER_EXPIRATION;
			e.src = STK_DEVICE_IDENTITY_TYPE_TERMINAL,
			e.timer_expiration.id = i + 1;
			timer_value_from_seconds(&e.timer_expiration.value,
							seconds);

			/*
			 * TODO: resubmit until success, providing current
			 * time difference every time we re-send.
			 */
			if (stk_send_envelope(stk, &e, timer_expiration_cb, 0))
				timer_expiration_cb(stk, FALSE, NULL, -1);

			continue;
		}

		if (stk->timers[i].expiry < now + min || min == 0)
			min = stk->timers[i].expiry - now;
	}

	if (min)
		stk->timers_source = g_timeout_add_seconds(min, timers_cb, stk);
}

static gboolean handle_command_timer_mgmt(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	int op = cmd->qualifier & 3;
	time_t seconds, now = time(NULL);
	struct stk_timer *tmr;

	if (cmd->timer_mgmt.timer_id < 1 || cmd->timer_mgmt.timer_id > 8) {
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	tmr = &stk->timers[cmd->timer_mgmt.timer_id - 1];

	switch (op) {
	case 0: /* Start */
		seconds = cmd->timer_mgmt.timer_value.second +
			cmd->timer_mgmt.timer_value.minute * 60 +
			cmd->timer_mgmt.timer_value.hour * 3600;

		tmr->expiry = now + seconds;
		tmr->start = now;

		timers_update(stk);
		break;

	case 1: /* Deactivate */
		if (!tmr->expiry) {
			rsp->result.type = STK_RESULT_TYPE_TIMER_CONFLICT;

			return TRUE;
		}

		seconds = MAX(0, tmr->expiry - now);
		tmr->expiry = 0;

		timers_update(stk);

		timer_value_from_seconds(&rsp->timer_mgmt.value, seconds);
		break;

	case 2: /* Get current value */
		if (!tmr->expiry) {
			rsp->result.type = STK_RESULT_TYPE_TIMER_CONFLICT;

			return TRUE;
		}

		seconds = MAX(0, tmr->expiry - now);
		timer_value_from_seconds(&rsp->timer_mgmt.value, seconds);
		break;

	default:
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;

		return TRUE;
	}

	rsp->timer_mgmt.id = cmd->timer_mgmt.timer_id;

	return TRUE;
}

static gboolean handle_command_poll_interval(const struct stk_command *cmd,
						struct stk_response *rsp,
						struct ofono_stk *stk)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	int seconds;

	switch (cmd->poll_interval.duration.unit) {
	case STK_DURATION_TYPE_MINUTES:
		seconds = cmd->poll_interval.duration.interval * 60;
		break;
	case STK_DURATION_TYPE_SECONDS:
		seconds = cmd->poll_interval.duration.interval;
		break;
	case STK_DURATION_TYPE_SECOND_TENTHS:
		seconds = (4 + cmd->poll_interval.duration.interval) / 10;
		if (seconds < 1)
			seconds = 1;
		break;
	default:
		rsp->result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		return TRUE;
	}

	if (seconds == 0)
		seconds = 30;

	ofono_modem_set_integer(modem, "status-poll-interval", seconds);

	if (seconds > 255) {
		rsp->poll_interval.max_interval.unit =
			STK_DURATION_TYPE_MINUTES;
		rsp->poll_interval.max_interval.interval = seconds / 60;
	} else {
		rsp->poll_interval.max_interval.unit =
			STK_DURATION_TYPE_SECONDS;
		rsp->poll_interval.max_interval.interval = seconds;
	}

	return TRUE;
}

static void stk_proactive_command_cancel(struct ofono_stk *stk)
{
	if (!stk->pending_cmd)
		return;

	stk->cancel_cmd(stk);

	if (stk->pending_cmd) {
		stk_command_free(stk->pending_cmd);
		stk->pending_cmd = NULL;
	}
}

void ofono_stk_proactive_session_end_notify(struct ofono_stk *stk)
{
	stk_proactive_command_cancel(stk);
}

void ofono_stk_proactive_command_notify(struct ofono_stk *stk,
					int length, const unsigned char *pdu)
{
	struct ofono_error error = { .type = OFONO_ERROR_TYPE_FAILURE };
	struct stk_response rsp;
	int err;
	gboolean respond = TRUE;

	/*
	 * Depending on the hardware we may have received a new
	 * command before we managed to send a TERMINAL RESPONSE to
	 * the previous one.  3GPP says in the current revision only
	 * one command can be executing at any time, so assume that
	 * the previous one is being cancelled and the card just
	 * expects a response to the new one.
	 */
	stk_proactive_command_cancel(stk);

	stk->pending_cmd = stk_command_new_from_pdu(pdu, length);
	if (!stk->pending_cmd) {
		ofono_error("Can't parse proactive command");

		/*
		 * Nothing we can do, we'd need at least Command Details
		 * to be able to respond with an error.
		 */
		return;
	}

	memset(&rsp, 0, sizeof(rsp));

	switch (stk->pending_cmd->status) {
	case STK_PARSE_RESULT_OK:
		switch (stk->pending_cmd->type) {
		default:
			rsp.result.type =
				STK_RESULT_TYPE_COMMAND_NOT_UNDERSTOOD;
			break;
		case STK_COMMAND_TYPE_MORE_TIME:
			respond = handle_command_more_time(stk->pending_cmd,
								&rsp, stk);
			break;
		case STK_COMMAND_TYPE_SEND_SMS:
			respond = handle_command_send_sms(stk->pending_cmd,
								&rsp, stk);
			break;
		case STK_COMMAND_TYPE_SETUP_IDLE_MODE_TEXT:
			respond = handle_command_set_idle_text(stk->pending_cmd,
								&rsp, stk);
			break;
		case STK_COMMAND_TYPE_TIMER_MANAGEMENT:
			respond = handle_command_timer_mgmt(stk->pending_cmd,
								&rsp, stk);
			break;
		case STK_COMMAND_TYPE_POLL_INTERVAL:
			respond = handle_command_poll_interval(stk->pending_cmd,
								&rsp, stk);
			break;
		}

		if (respond)
			break;
		return;

	case STK_PARSE_RESULT_MISSING_VALUE:
		rsp.result.type = STK_RESULT_TYPE_MINIMUM_NOT_MET;
		break;

	case STK_PARSE_RESULT_DATA_NOT_UNDERSTOOD:
		rsp.result.type = STK_RESULT_TYPE_DATA_NOT_UNDERSTOOD;
		break;

	case STK_PARSE_RESULT_TYPE_NOT_UNDERSTOOD:
	default:
		rsp.result.type = STK_RESULT_TYPE_COMMAND_NOT_UNDERSTOOD;
		break;
	}

	err = stk_respond(stk, &rsp, stk_command_cb);
	if (err)
		stk_command_cb(&error, stk);
}

int ofono_stk_driver_register(const struct ofono_stk_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_stk_driver_unregister(const struct ofono_stk_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void stk_unregister(struct ofono_atom *atom)
{
	struct ofono_stk *stk = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	if (stk->pending_cmd) {
		stk_command_free(stk->pending_cmd);
		stk->pending_cmd = NULL;
	}

	if (stk->idle_mode_text) {
		g_free(stk->idle_mode_text);
		stk->idle_mode_text = NULL;
	}

	if (stk->timers_source) {
		g_source_remove(stk->timers_source);
		stk->timers_source = 0;
	}

	g_queue_foreach(stk->envelope_q, (GFunc) g_free, NULL);
	g_queue_free(stk->envelope_q);

	ofono_modem_remove_interface(modem, OFONO_STK_INTERFACE);
	g_dbus_unregister_interface(conn, path, OFONO_STK_INTERFACE);
}

static void stk_remove(struct ofono_atom *atom)
{
	struct ofono_stk *stk = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (stk == NULL)
		return;

	if (stk->driver && stk->driver->remove)
		stk->driver->remove(stk);

	g_free(stk);
}

struct ofono_stk *ofono_stk_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver,
					void *data)
{
	struct ofono_stk *stk;
	GSList *l;

	if (driver == NULL)
		return NULL;

	stk = g_try_new0(struct ofono_stk, 1);

	if (stk == NULL)
		return NULL;

	stk->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_STK,
						stk_remove, stk);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_stk_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(stk, vendor, data) < 0)
			continue;

		stk->driver = drv;
		break;
	}

	return stk;
}

void ofono_stk_register(struct ofono_stk *stk)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(stk->atom);
	const char *path = __ofono_atom_get_path(stk->atom);

	if (!g_dbus_register_interface(conn, path, OFONO_STK_INTERFACE,
					stk_methods, stk_signals, NULL,
					stk, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_STK_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, OFONO_STK_INTERFACE);

	__ofono_atom_register(stk->atom, stk_unregister);

	stk->envelope_q = g_queue_new();
}

void ofono_stk_remove(struct ofono_stk *stk)
{
	__ofono_atom_free(stk->atom);
}

void ofono_stk_set_data(struct ofono_stk *stk, void *data)
{
	stk->driver_data = data;
}

void *ofono_stk_get_data(struct ofono_stk *stk)
{
	return stk->driver_data;
}
