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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>

#include "ofono.h"
#include "common.h"
#include "gatserver.h"
#include "gatppp.h"

#define RING_TIMEOUT 3

struct ofono_emulator {
	struct ofono_atom *atom;
	enum ofono_emulator_type type;
	GAtServer *server;
	GAtPPP *ppp;
	gboolean slc;
	int l_features;
	int r_features;
	int events_mode;
	gboolean events_ind;
	unsigned char cmee_mode;
	GSList *indicators;
	guint callsetup_source;
	gboolean clip;
	gboolean ccwa;
	int pns_id;
};

struct indicator {
	char *name;
	int value;
	int min;
	int max;
	gboolean deferred;
	gboolean active;
	gboolean mandatory;
};

static void emulator_debug(const char *str, void *data)
{
	ofono_info("%s: %s\n", (char *)data, str);
}

static void emulator_disconnect(gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	DBG("%p", em);

	ofono_emulator_remove(em);
}

static void ppp_connect(const char *iface, const char *local,
			const char *remote,
			const char *dns1, const char *dns2,
			gpointer user_data)
{
	DBG("Network Device: %s\n", iface);
	DBG("IP Address: %s\n", local);
	DBG("Remote IP Address: %s\n", remote);
	DBG("Primary DNS Server: %s\n", dns1);
	DBG("Secondary DNS Server: %s\n", dns2);
}

static void cleanup_ppp(struct ofono_emulator *em)
{
	DBG("");

	g_at_ppp_unref(em->ppp);
	em->ppp = NULL;

	__ofono_private_network_release(em->pns_id);
	em->pns_id = 0;

	g_at_server_resume(em->server);
	g_at_server_send_final(em->server, G_AT_SERVER_RESULT_NO_CARRIER);
}

static void ppp_disconnect(GAtPPPDisconnectReason reason, gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	cleanup_ppp(em);
}

static void ppp_suspend(gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	DBG("");

	g_at_server_resume(em->server);
}

static void suspend_server(gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtIO *io = g_at_server_get_io(em->server);

	g_at_server_suspend(em->server);

	if (g_at_ppp_listen(em->ppp, io) == FALSE)
		cleanup_ppp(em);
}

static void request_private_network_cb(
			const struct ofono_private_network_settings *pns,
			void *data)
{
	struct ofono_emulator *em = data;
	GAtIO *io = g_at_server_get_io(em->server);

	if (pns == NULL)
		goto error;

	em->ppp = g_at_ppp_server_new_full(pns->server_ip, pns->fd);
	if (em->ppp == NULL) {
		close(pns->fd);
		goto badalloc;
	}

	g_at_ppp_set_server_info(em->ppp, pns->peer_ip,
					pns->primary_dns, pns->secondary_dns);

	g_at_ppp_set_acfc_enabled(em->ppp, TRUE);
	g_at_ppp_set_pfc_enabled(em->ppp, TRUE);

	g_at_ppp_set_credentials(em->ppp, "", "");
	g_at_ppp_set_debug(em->ppp, emulator_debug, "PPP");

	g_at_ppp_set_connect_function(em->ppp, ppp_connect, em);
	g_at_ppp_set_disconnect_function(em->ppp, ppp_disconnect, em);
	g_at_ppp_set_suspend_function(em->ppp, ppp_suspend, em);

	g_at_server_send_intermediate(em->server, "CONNECT");
	g_at_io_set_write_done(io, suspend_server, em);

	return;

badalloc:
	__ofono_private_network_release(em->pns_id);

error:
	em->pns_id = 0;
	g_at_server_send_final(em->server, G_AT_SERVER_RESULT_ERROR);
}

static gboolean dial_call(struct ofono_emulator *em, const char *dial_str)
{
	char c = *dial_str;

	DBG("dial call %s", dial_str);

	if (c == '*' || c == '#' || c == 'T' || c == 't') {
		if (__ofono_private_network_request(request_private_network_cb,
						&em->pns_id, em) == FALSE)
			return FALSE;
	}

	return TRUE;
}

static void dial_cb(GAtServer *server, GAtServerRequestType type,
				GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	const char *dial_str;

	DBG("");

	if (type != G_AT_SERVER_REQUEST_TYPE_SET)
		goto error;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, ""))
		goto error;

	dial_str = g_at_result_iter_raw_line(&iter);
	if (!dial_str)
		goto error;

	if (em->ppp)
		goto error;

	if (!dial_call(em, dial_str))
		goto error;

	return;

error:
	g_at_server_send_final(em->server, G_AT_SERVER_RESULT_ERROR);
}

static void dun_ath_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;

	DBG("");

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto error;

		if (val != 0)
			goto error;

		/* Fall through */

	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		if (em->ppp == NULL)
			goto error;

		g_at_ppp_unref(em->ppp);
		em->ppp = NULL;

		__ofono_private_network_release(em->pns_id);
		em->pns_id = 0;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
error:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void resume_ppp(gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	g_at_server_suspend(em->server);
	g_at_ppp_resume(em->ppp);
}

static void dun_ato_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtIO *io = g_at_server_get_io(em->server);
	GAtResultIter iter;
	int val;

	DBG("");

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto error;

		if (val != 0)
			goto error;

		/* Fall through */
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		if (em->ppp == NULL)
			goto error;

		g_at_server_send_intermediate(em->server, "CONNECT");
		g_at_io_set_write_done(io, resume_ppp, em);
		break;

	default:
error:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static struct indicator *find_indicator(struct ofono_emulator *em,
						const char *name, int *index)
{
	GSList *l;
	int i;

	for (i = 1, l = em->indicators; l; l = l->next, i++) {
		struct indicator *ind = l->data;

		if (g_str_equal(ind->name, name) == FALSE)
			continue;

		if (index)
			*index = i;

		return ind;
	}

	return NULL;
}

static struct ofono_call *find_call_with_status(struct ofono_emulator *em,
								int status)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(em->atom);
	struct ofono_voicecall *vc;

	vc = __ofono_atom_find(OFONO_ATOM_TYPE_VOICECALL, modem);
	if (vc == NULL)
		return NULL;

	return __ofono_voicecall_find_call_with_status(vc, status);
}

static void notify_deferred_indicators(GAtServer *server, void *user_data)
{
	struct ofono_emulator *em = user_data;
	int i;
	char buf[20];
	GSList *l;
	struct indicator *ind;

	for (i = 1, l = em->indicators; l; l = l->next, i++) {
		ind = l->data;

		if (!ind->deferred)
			continue;

		if (em->events_mode == 3 && em->events_ind && em->slc &&
				ind->active) {
			sprintf(buf, "+CIEV: %d,%d", i, ind->value);
			g_at_server_send_unsolicited(em->server, buf);
		}

		ind->deferred = FALSE;
	}
}

static gboolean notify_ccwa(void *user_data)
{
	struct ofono_emulator *em = user_data;
	struct ofono_call *c;
	const char *phone;
	/*
	 * '+CCWA: "+",' + phone number + phone type on 3 digits max
	 * + terminating null
	 */
	char str[OFONO_MAX_PHONE_NUMBER_LENGTH + 14 + 1];

	if ((em->type == OFONO_EMULATOR_TYPE_HFP && em->slc == FALSE) ||
			!em->ccwa)
		goto end;

	c = find_call_with_status(em, CALL_STATUS_WAITING);

	if (c && c->clip_validity == CLIP_VALIDITY_VALID) {
		phone = phone_number_to_string(&c->phone_number);
		sprintf(str, "+CCWA: \"%s\",%d", phone, c->phone_number.type);

		g_at_server_send_unsolicited(em->server, str);
	} else
		g_at_server_send_unsolicited(em->server, "+CCWA: \"\",128");

end:
	em->callsetup_source = 0;

	return FALSE;
}

static gboolean notify_ring(void *user_data)
{
	struct ofono_emulator *em = user_data;
	struct ofono_call *c;
	const char *phone;
	/*
	 * '+CLIP: "+",' + phone number + phone type on 3 digits max
	 * + terminating null
	 */
	char str[OFONO_MAX_PHONE_NUMBER_LENGTH + 14 + 1];

	if (em->type == OFONO_EMULATOR_TYPE_HFP && em->slc == FALSE)
		return TRUE;

	g_at_server_send_unsolicited(em->server, "RING");

	if (!em->clip)
		return TRUE;

	c = find_call_with_status(em, CALL_STATUS_INCOMING);

	if (c == NULL)
		return TRUE;

	switch (c->clip_validity) {
	case CLIP_VALIDITY_VALID:
		phone = phone_number_to_string(&c->phone_number);
		sprintf(str, "+CLIP: \"%s\",%d", phone, c->phone_number.type);
		g_at_server_send_unsolicited(em->server, str);
		break;

	case CLIP_VALIDITY_WITHHELD:
		g_at_server_send_unsolicited(em->server, "+CLIP: \"\",128");
		break;
	}

	return TRUE;
}

static void brsf_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;
	char buf[16];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto fail;

		if (val < 0 || val > 127)
			goto fail;

		em->r_features = val;

		sprintf(buf, "+BRSF: %d", em->l_features);
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cind_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GSList *l;
	struct indicator *ind;
	gsize size;
	int len;
	char *buf;
	char *tmp;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		/*
		 * "+CIND: " + terminating null + number of indicators *
		 * (max of 3 digits in the value + separator)
		 */
		size = 7 + 1 + (g_slist_length(em->indicators) * 4);
		buf = g_try_malloc0(size);
		if (buf == NULL)
			goto fail;

		len = sprintf(buf, "+CIND: ");
		tmp = buf + len;

		for (l = em->indicators; l; l = l->next) {
			ind = l->data;
			len = sprintf(tmp, "%s%d",
					l == em->indicators ? "" : ",",
					ind->value);
			tmp = tmp + len;
		}

		g_at_server_send_info(em->server, buf, TRUE);
		g_free(buf);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		/*
		 * '+CIND: ' + terminating null + number of indicators *
		 * ( indicator name + '("",(000,000))' + separator)
		 */
		size = 8;

		for (l = em->indicators; l; l = l->next) {
			ind = l->data;
			size += strlen(ind->name) + 15;
		}

		buf = g_try_malloc0(size);
		if (buf == NULL)
			goto fail;

		len = sprintf(buf, "+CIND: ");
		tmp = buf + len;

		for (l = em->indicators; l; l = l->next) {
			ind = l->data;
			len = sprintf(tmp, "%s(\"%s\",(%d%c%d))",
					l == em->indicators ? "" : ",",
					ind->name, ind->min,
					(ind->max - ind->min) == 1 ? ',' : '-',
					ind->max);
			tmp = tmp + len;
		}

		g_at_server_send_info(server, buf, TRUE);
		g_free(buf);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void cmer_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	char buf[32];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		sprintf(buf, "+CMER: %d,0,0,%d,0", em->events_mode,
						em->events_ind);
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		sprintf(buf, "+CMER: (0,3),(0),(0),(0,1),(0)");
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		int mode = em->events_mode;
		int ind = em->events_ind;
		int val;

		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		/* mode */
		if (!g_at_result_iter_next_number_default(&iter, mode, &mode))
			goto fail;

		if (mode != 0 && mode != 3)
			goto fail;

		/* keyp */
		if (!g_at_result_iter_next_number_default(&iter, 0, &val)) {
			if (!g_at_result_iter_skip_next(&iter))
				goto done;
			goto fail;
		}

		if (val != 0)
			goto fail;

		/* disp */
		if (!g_at_result_iter_next_number_default(&iter, 0, &val)) {
			if (!g_at_result_iter_skip_next(&iter))
				goto done;
			goto fail;
		}

		if (val != 0)
			goto fail;

		/* ind */
		if (!g_at_result_iter_next_number_default(&iter, ind, &ind)) {
			if (!g_at_result_iter_skip_next(&iter))
				goto done;
			goto fail;
		}

		if (ind != 0 && ind != 1)
			goto fail;

		/* bfr */
		if (!g_at_result_iter_next_number_default(&iter, 0, &val)) {
			if (!g_at_result_iter_skip_next(&iter))
				goto done;
			goto fail;
		}

		if (val != 0)
			goto fail;

		/* check that bfr is last parameter */
		if (g_at_result_iter_skip_next(&iter))
			goto fail;

done:
		em->events_mode = mode;
		em->events_ind = ind;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);

		em->slc = TRUE;
		break;
	}

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void clip_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;

	if (em->slc == FALSE)
		goto fail;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (!g_at_result_iter_next_number(&iter, &val))
			goto fail;

		if (val != 0 && val != 1)
			goto fail;

		/* check this is last parameter */
		if (g_at_result_iter_skip_next(&iter))
			goto fail;

		em->clip = val;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void ccwa_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;
	struct indicator *call_ind;
	struct indicator *cs_ind;

	if (em->slc == FALSE)
		goto fail;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (!g_at_result_iter_next_number(&iter, &val))
			goto fail;

		if (val != 0 && val != 1)
			goto fail;

		/* check this is last parameter */
		if (g_at_result_iter_skip_next(&iter))
			goto fail;

		call_ind = find_indicator(em, OFONO_EMULATOR_IND_CALL, NULL);
		cs_ind = find_indicator(em, OFONO_EMULATOR_IND_CALLSETUP, NULL);

		if (cs_ind->value == OFONO_EMULATOR_CALLSETUP_INCOMING &&
				call_ind->value == OFONO_EMULATOR_CALL_ACTIVE &&
				em->ccwa == FALSE && val == 1)
			em->callsetup_source = g_timeout_add_seconds(0,
							notify_ccwa, em);

		em->ccwa = val;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
	};
}

static void cmee_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;
	GAtResultIter iter;
	int val;
	char buf[16];

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		if (g_at_result_iter_next_number(&iter, &val) == FALSE)
			goto fail;

		if (val != 0 && val != 1)
			goto fail;

		em->cmee_mode = val;

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		sprintf(buf, "+CMEE: %d", em->cmee_mode);
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		/* HFP only support 0 and 1 */
		sprintf(buf, "+CMEE: (0,1)");
		g_at_server_send_info(em->server, buf, TRUE);
		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void bia_cb(GAtServer *server, GAtServerRequestType type,
			GAtResult *result, gpointer user_data)
{
	struct ofono_emulator *em = user_data;

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_SET:
	{
		GAtResultIter iter;
		GSList *l;
		int val;

		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		/* check validity of the request */
		while (g_at_result_iter_next_number_default(&iter, 0, &val))
			if (val != 0 &&  val != 1)
				goto fail;

		/* Check that we have no non-numbers in the stream */
		if (g_at_result_iter_skip_next(&iter) == TRUE)
			goto fail;

		/* request is valid, update the indicator activation status */
		g_at_result_iter_init(&iter, result);
		g_at_result_iter_next(&iter, "");

		for (l = em->indicators; l; l = l->next) {
			struct indicator *ind = l->data;

			if (g_at_result_iter_next_number_default(&iter,
						ind->active, &val) == FALSE)
				break;

			if (ind->mandatory == TRUE)
				continue;

			ind->active = val;
		}

		g_at_server_send_final(server, G_AT_SERVER_RESULT_OK);
		break;
	}

	default:
fail:
		g_at_server_send_final(server, G_AT_SERVER_RESULT_ERROR);
		break;
	}
}

static void emulator_add_indicator(struct ofono_emulator *em, const char* name,
					int min, int max, int dflt,
					gboolean mandatory)
{
	struct indicator *ind;

	ind = g_try_new0(struct indicator, 1);
	if (ind == NULL) {
		ofono_error("Unable to allocate indicator structure");
		return;
	}

	ind->name = g_strdup(name);
	ind->min = min;
	ind->max = max;
	ind->value = dflt;
	ind->active = TRUE;
	ind->mandatory = mandatory;

	em->indicators = g_slist_append(em->indicators, ind);
}

static void emulator_unregister(struct ofono_atom *atom)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);
	GSList *l;

	DBG("%p", em);

	if (em->callsetup_source) {
		g_source_remove(em->callsetup_source);
		em->callsetup_source = 0;
	}

	for (l = em->indicators; l; l = l->next) {
		struct indicator *ind = l->data;

		g_free(ind->name);
		g_free(ind);
	}

	g_slist_free(em->indicators);
	em->indicators = NULL;

	g_at_ppp_unref(em->ppp);
	em->ppp = NULL;

	if (em->pns_id > 0) {
		__ofono_private_network_release(em->pns_id);
		em->pns_id = 0;
	}

	g_at_server_unref(em->server);
	em->server = NULL;
}

void ofono_emulator_register(struct ofono_emulator *em, int fd)
{
	GIOChannel *io;

	DBG("%p, %d", em, fd);

	if (fd < 0)
		return;

	io = g_io_channel_unix_new(fd);

	em->server = g_at_server_new(io);
	if (em->server == NULL)
		return;

	g_io_channel_unref(io);

	g_at_server_set_debug(em->server, emulator_debug, "Server");
	g_at_server_set_disconnect_function(em->server,
						emulator_disconnect, em);
	g_at_server_set_finish_callback(em->server, notify_deferred_indicators,
						em);

	if (em->type == OFONO_EMULATOR_TYPE_HFP) {
		emulator_add_indicator(em, OFONO_EMULATOR_IND_SERVICE, 0, 1, 0,
									FALSE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_CALL, 0, 1, 0,
									TRUE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_CALLSETUP, 0, 3,
								0, TRUE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_CALLHELD, 0, 2,
								0, TRUE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_SIGNAL, 0, 5, 0,
									FALSE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_ROAMING, 0, 1, 0,
									FALSE);
		emulator_add_indicator(em, OFONO_EMULATOR_IND_BATTERY, 0, 5, 5,
									FALSE);

		g_at_server_register(em->server, "+BRSF", brsf_cb, em, NULL);
		g_at_server_register(em->server, "+CIND", cind_cb, em, NULL);
		g_at_server_register(em->server, "+CMER", cmer_cb, em, NULL);
		g_at_server_register(em->server, "+CLIP", clip_cb, em, NULL);
		g_at_server_register(em->server, "+CCWA", ccwa_cb, em, NULL);
		g_at_server_register(em->server, "+CMEE", cmee_cb, em, NULL);
		g_at_server_register(em->server, "+BIA", bia_cb, em, NULL);
	}

	__ofono_atom_register(em->atom, emulator_unregister);

	switch (em->type) {
	case OFONO_EMULATOR_TYPE_DUN:
		g_at_server_register(em->server, "D", dial_cb, em, NULL);
		g_at_server_register(em->server, "H", dun_ath_cb, em, NULL);
		g_at_server_register(em->server, "O", dun_ato_cb, em, NULL);
		break;
	case OFONO_EMULATOR_TYPE_HFP:
		g_at_server_set_echo(em->server, FALSE);
		break;
	default:
		break;
	}
}

static void emulator_remove(struct ofono_atom *atom)
{
	struct ofono_emulator *em = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	g_free(em);
}

struct ofono_emulator *ofono_emulator_create(struct ofono_modem *modem,
						enum ofono_emulator_type type)
{
	struct ofono_emulator *em;
	enum ofono_atom_type atom_t;

	DBG("modem: %p, type: %d", modem, type);

	if (type == OFONO_EMULATOR_TYPE_DUN)
		atom_t = OFONO_ATOM_TYPE_EMULATOR_DUN;
	else if (type == OFONO_EMULATOR_TYPE_HFP)
		atom_t = OFONO_ATOM_TYPE_EMULATOR_HFP;
	else
		return NULL;

	em = g_try_new0(struct ofono_emulator, 1);

	if (em == NULL)
		return NULL;

	em->type = type;
	em->l_features |= HFP_AG_FEATURE_3WAY;
	em->l_features |= HFP_AG_FEATURE_REJECT_CALL;
	em->l_features |= HFP_AG_FEATURE_ENHANCED_CALL_STATUS;
	em->l_features |= HFP_AG_FEATURE_ENHANCED_CALL_CONTROL;
	em->l_features |= HFP_AG_FEATURE_EXTENDED_RES_CODE;
	em->events_mode = 3;	/* default mode is forwarding events */
	em->cmee_mode = 0;	/* CME ERROR disabled by default */

	em->atom = __ofono_modem_add_atom_offline(modem, atom_t,
							emulator_remove, em);

	return em;
}

void ofono_emulator_remove(struct ofono_emulator *em)
{
	__ofono_atom_free(em->atom);
}

void ofono_emulator_send_final(struct ofono_emulator *em,
				const struct ofono_error *final)
{
	char buf[256];

	/*
	 * TODO: Handle various CMEE modes and report error strings from
	 * common.c
	 */
	switch (final->type) {
	case OFONO_ERROR_TYPE_CMS:
		sprintf(buf, "+CMS ERROR: %d", final->error);
		g_at_server_send_ext_final(em->server, buf);
		break;

	case OFONO_ERROR_TYPE_CME:
		switch (em->cmee_mode) {
		case 1:
			sprintf(buf, "+CME ERROR: %d", final->error);
			break;

		case 2:
			sprintf(buf, "+CME ERROR: %s",
						telephony_error_to_str(final));
			break;

		default:
			goto failure;
		}

		g_at_server_send_ext_final(em->server, buf);
		break;

	case OFONO_ERROR_TYPE_NO_ERROR:
		g_at_server_send_final(em->server, G_AT_SERVER_RESULT_OK);
		break;

	case OFONO_ERROR_TYPE_CEER:
	case OFONO_ERROR_TYPE_SIM:
	case OFONO_ERROR_TYPE_FAILURE:
failure:
		g_at_server_send_final(em->server, G_AT_SERVER_RESULT_ERROR);
		break;
	};
}

void ofono_emulator_send_unsolicited(struct ofono_emulator *em,
					const char *result)
{
	g_at_server_send_unsolicited(em->server, result);
}

void ofono_emulator_send_intermediate(struct ofono_emulator *em,
					const char *result)
{
	g_at_server_send_intermediate(em->server, result);
}

void ofono_emulator_send_info(struct ofono_emulator *em, const char *line,
				ofono_bool_t last)
{
	g_at_server_send_info(em->server, line, last);
}

struct handler {
	ofono_emulator_request_cb_t cb;
	void *data;
	ofono_destroy_func destroy;
	struct ofono_emulator *em;
};

struct ofono_emulator_request {
	GAtResultIter iter;
	enum ofono_emulator_request_type type;
};

static void handler_proxy(GAtServer *server, GAtServerRequestType type,
				GAtResult *result, gpointer userdata)
{
	struct handler *h = userdata;
	struct ofono_emulator_request req;

	if (h->em->type == OFONO_EMULATOR_TYPE_HFP && h->em->slc == FALSE) {
		g_at_server_send_final(h->em->server, G_AT_SERVER_RESULT_ERROR);
		return;
	}

	switch (type) {
	case G_AT_SERVER_REQUEST_TYPE_COMMAND_ONLY:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_COMMAND_ONLY;
		break;
	case G_AT_SERVER_REQUEST_TYPE_SET:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_SET;
		break;
	case G_AT_SERVER_REQUEST_TYPE_QUERY:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_QUERY;
		break;
	case G_AT_SERVER_REQUEST_TYPE_SUPPORT:
		req.type = OFONO_EMULATOR_REQUEST_TYPE_SUPPORT;
	}

	g_at_result_iter_init(&req.iter, result);
	g_at_result_iter_next(&req.iter, "");

	h->cb(h->em, &req, h->data);
}

static void handler_destroy(gpointer userdata)
{
	struct handler *h = userdata;

	if (h->destroy)
		h->destroy(h->data);

	g_free(h);
}

ofono_bool_t ofono_emulator_add_handler(struct ofono_emulator *em,
					const char *prefix,
					ofono_emulator_request_cb_t cb,
					void *data, ofono_destroy_func destroy)
{
	struct handler *h;

	h = g_new0(struct handler, 1);
	h->cb = cb;
	h->data = data;
	h->destroy = destroy;
	h->em = em;

	if (g_at_server_register(em->server, prefix, handler_proxy, h,
					handler_destroy) == TRUE)
		return TRUE;

	g_free(h);

	return FALSE;
}

ofono_bool_t ofono_emulator_remove_handler(struct ofono_emulator *em,
						const char *prefix)
{
	return g_at_server_unregister(em->server, prefix);
}

ofono_bool_t ofono_emulator_request_next_string(
					struct ofono_emulator_request *req,
					const char **str)
{
	return g_at_result_iter_next_string(&req->iter, str);
}

ofono_bool_t ofono_emulator_request_next_number(
					struct ofono_emulator_request *req,
					int *number)
{
	return g_at_result_iter_next_number(&req->iter, number);
}

const char *ofono_emulator_request_get_raw(struct ofono_emulator_request *req)
{
	return g_at_result_iter_raw_line(&req->iter);
}

enum ofono_emulator_request_type ofono_emulator_request_get_type(
					struct ofono_emulator_request *req)
{
	return req->type;
}

void ofono_emulator_set_indicator(struct ofono_emulator *em,
					const char *name, int value)
{
	int i;
	char buf[20];
	struct indicator *ind;
	struct indicator *call_ind;
	struct indicator *cs_ind;
	gboolean call;
	gboolean callsetup;
	gboolean waiting;

	ind = find_indicator(em, name, &i);

	if (ind == NULL || ind->value == value || value < ind->min
			|| value > ind->max)
		return;

	ind->value = value;

	call_ind = find_indicator(em, OFONO_EMULATOR_IND_CALL, NULL);
	cs_ind = find_indicator(em, OFONO_EMULATOR_IND_CALLSETUP, NULL);

	call = ind == call_ind;
	callsetup = ind == cs_ind;

	/*
	 * When callsetup indicator goes to Incoming and there is an active
	 * call a +CCWA should be sent before +CIEV
	 */
	waiting = (callsetup && value == OFONO_EMULATOR_CALLSETUP_INCOMING &&
			call_ind->value == OFONO_EMULATOR_CALL_ACTIVE);

	if (waiting)
		notify_ccwa(em);

	if (em->events_mode == 3 && em->events_ind && em->slc && ind->active) {
		if (!g_at_server_command_pending(em->server)) {
			sprintf(buf, "+CIEV: %d,%d", i, ind->value);
			g_at_server_send_unsolicited(em->server, buf);
		} else
			ind->deferred = TRUE;
	}

	/*
	 * Ring timer should be started when:
	 * - callsetup indicator is set to Incoming and there is no active call
	 *   (not a waiting call)
	 * - or call indicator is set to inactive while callsetup is already
	 *   set to Incoming.
	 * In those cases, a first RING should be sent just after the +CIEV
	 * Ring timer should be stopped for all other values of callsetup
	 */
	if (waiting)
		return;

	/* Call state went from active/held + waiting -> incoming */
	if (call && value == OFONO_EMULATOR_CALL_INACTIVE &&
			cs_ind->value == OFONO_EMULATOR_CALLSETUP_INCOMING)
		goto start_ring;

	if (!callsetup)
		return;

	if (value != OFONO_EMULATOR_CALLSETUP_INCOMING) {
		if (em->callsetup_source > 0) {
			g_source_remove(em->callsetup_source);
			em->callsetup_source = 0;
		}

		return;
	}

start_ring:
	notify_ring(em);
	em->callsetup_source = g_timeout_add_seconds(RING_TIMEOUT,
							notify_ring, em);
}

void __ofono_emulator_set_indicator_forced(struct ofono_emulator *em,
						const char *name, int value)
{
	int i;
	struct indicator *ind;
	char buf[20];

	ind = find_indicator(em, name, &i);

	if (ind == NULL || value < ind->min || value > ind->max)
		return;

	ind->value = value;

	if (em->events_mode == 3 && em->events_ind && em->slc && ind->active) {
		if (!g_at_server_command_pending(em->server)) {
			sprintf(buf, "+CIEV: %d,%d", i, ind->value);
			g_at_server_send_unsolicited(em->server, buf);
		} else
			ind->deferred = TRUE;
	}
}
