/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>
#include <ofono/gprs-context.h>
#include <gisi/client.h>
#include <gisi/iter.h>

#include "isimodem.h"
#include "isiutil.h"
#include "gpds.h"
#include "info.h"
#include "debug.h"

/* 27.007 Section 10.1.20 <stat> */
enum network_registration_status {
	GPRS_STAT_NOT_REGISTERED =	0,
	GPRS_STAT_REGISTERED =		1,
	GPRS_STAT_SEARCHING =		2,
	GPRS_STAT_DENIED =		3,
	GPRS_STAT_UNKNOWN =		4,
	GPRS_STAT_ROAMING =		5,
};

struct gprs_data {
	GIsiClient *client;
	GIsiClient *info_client;
};

static void configure_resp_cb(const GIsiMessage *msg, void *opaque)
{
	const uint8_t *data = g_isi_msg_data(msg);

	if (g_isi_msg_error(msg) < 0) {
		DBG("ISI message error: %d", g_isi_msg_error(msg));
		return;
	}

	if (g_isi_msg_id(msg) != GPDS_CONFIGURE_RESP)
		return;

	if (g_isi_msg_data_len(msg) < 1)
		return;

	if (data[0] != GPDS_OK)
		DBG("GPDS configure failed: %s", gpds_status_name(data[0]));
}

static void set_attach_mode(struct ofono_gprs *gprs, int attached)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	const unsigned char msg[] = {
		GPDS_CONFIGURE_REQ,
		attached ? GPDS_ATTACH_MODE_AUTOMATIC : GPDS_ATTACH_MODE_MANUAL,
		GPDS_MT_ACT_MODE_REJECT,
		GPDS_CLASSC_MODE_DEFAULT,
		GPDS_AOL_CTX_DEFAULT,
		0x00,
		0x00
	};

	g_isi_client_send(gd->client, msg, sizeof(msg),
				configure_resp_cb, gprs, NULL);
}

static void detach_ind_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_gprs *gprs = opaque;
	const uint8_t *data = g_isi_msg_data(msg);

	if (g_isi_msg_error(msg) < 0)
		return;

	if (g_isi_msg_id(msg) != GPDS_DETACH_IND)
		return;

	if (g_isi_msg_data_len(msg) < 2)
		return;

	DBG("detached: %s (0x%02"PRIx8")",
		gpds_isi_cause_name(data[0]), data[0]);

	set_attach_mode(gprs, FALSE);

	ofono_gprs_detached_notify(gprs);
}

static void suspend_notify(struct ofono_gprs *gprs, uint8_t suspend_status,
			uint8_t suspend_cause)
{
	int cause;

	DBG("transfer status: %s (0x%02"PRIx8") cause %s (0x%02"PRIx8")",
		gpds_transfer_status_name(suspend_status), suspend_status,
		gpds_transfer_cause_name(suspend_cause), suspend_cause);

	if (suspend_status == GPDS_TRANSFER_AVAIL) {
		ofono_gprs_resume_notify(gprs);
		return;
	}

	switch (suspend_cause) {
	case GPDS_TRANSFER_CAUSE_SUSPENDED_NO_COVERAGE:
		cause = GPRS_SUSPENDED_NO_COVERAGE;
		break;

	case GPDS_TRANSFER_CAUSE_SUSPENDED_CALL:
		cause = GPRS_SUSPENDED_CALL;
		break;

	case GPDS_TRANSFER_CAUSE_SUSPENDED_CALL_SMS:
	case GPDS_TRANSFER_CAUSE_SUSPENDED_RAU:
	case GPDS_TRANSFER_CAUSE_SUSPENDED_LU:
		cause = GPRS_SUSPENDED_SIGNALLING;
		break;

	default:
		return;
	}

	ofono_gprs_suspend_notify(gprs, cause);
}

static void transfer_status_ind_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_gprs *gprs = opaque;
	const uint8_t *data = g_isi_msg_data(msg);

	if (g_isi_msg_error(msg) < 0)
		return;

	if (g_isi_msg_id(msg) != GPDS_TRANSFER_STATUS_IND)
		return;

	if (g_isi_msg_data_len(msg) < 2)
		return;

	suspend_notify(gprs, data[0], data[1]);
}

static void create_contexts(struct ofono_gprs *gprs, int count)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	GIsiModem *modem = g_isi_client_modem(gd->client);
	struct ofono_modem *omodem = g_isi_modem_get_userdata(modem);
	struct ofono_gprs_context *gc;
	int i;

	for (i = 0; i < count; i++) {
		gc = ofono_gprs_context_create(omodem, 0, "isimodem", modem);
		if (gc == NULL)
			break;

		ofono_gprs_add_context(gprs, gc);
	}

	ofono_gprs_set_cid_range(gprs, 1, i);

	DBG("%d GPRS contexts created", count);
}

static void info_pp_read_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_gprs *gprs = opaque;
	uint8_t count = GPDS_MAX_CONTEXT_COUNT;
	GIsiSubBlockIter iter;

	if (g_isi_msg_error(msg) == -ESHUTDOWN)
		return;

	if (g_isi_msg_error(msg) < 0)
		goto out;

	if (g_isi_msg_id(msg) != INFO_PP_READ_RESP)
		goto out;

	for (g_isi_sb_iter_init(&iter, msg, 2); g_isi_sb_iter_is_valid(&iter);
			g_isi_sb_iter_next(&iter)) {

		switch (g_isi_sb_iter_get_id(&iter)) {
		case INFO_SB_PP: {
			guint16 fea;
			guint8 n;
			unsigned pp;

			if (!g_isi_sb_iter_get_byte(&iter, &n, 1))
				goto out;

			for (pp = 4; n--; pp += 2) {

				if (!g_isi_sb_iter_get_word(&iter, &fea, pp))
					goto out;

				if ((fea >> 8) != INFO_PP_MAX_PDP_CONTEXTS)
					goto out;

				count = fea & 0xff;
				break;
			}
			break;
		}

		default:
			break;
		}
	}

out:
	create_contexts(gprs, count);
}

static void gpds_reachable_cb(const GIsiMessage *msg, void *opaque)
{
	struct ofono_gprs *gprs = opaque;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	GIsiModem *modem = g_isi_client_modem(gd->client);

	const unsigned char req[] = {
		INFO_PP_READ_REQ,
		0,				/* filler */
		1,				/* subblocks */
		INFO_SB_PP,
		8,				/* subblock length */
		0,
		1,				/* N */
		INFO_PP_MAX_PDP_CONTEXTS,	/* PP feature */
		0,				/* PP value */
		0,				/* filler */
		0				/* filler */
	};

	if (g_isi_msg_error(msg) < 0) {
		DBG("unable to bootstrap gprs driver");
		ofono_gprs_remove(gprs);
		return;
	}

	ISI_RESOURCE_DBG(msg);

	g_isi_client_ind_subscribe(gd->client, GPDS_DETACH_IND,
					detach_ind_cb, gprs);
	g_isi_client_ind_subscribe(gd->client, GPDS_TRANSFER_STATUS_IND,
					transfer_status_ind_cb, gprs);

	ofono_gprs_register(gprs);

	gd->info_client = g_isi_client_create(modem, PN_PHONE_INFO);
	if (gd->info_client == NULL) {
		create_contexts(gprs, GPDS_MAX_CONTEXT_COUNT);
		return;
	}

	g_isi_client_send(gd->info_client, req, sizeof(req),
				info_pp_read_resp_cb, gprs, NULL);
}

static int isi_gprs_probe(struct ofono_gprs *gprs,
				unsigned int vendor, void *user)
{
	GIsiModem *modem = user;
	struct gprs_data *gd = g_try_new0(struct gprs_data, 1);

	if (gd == NULL)
		return -ENOMEM;

	gd->client = g_isi_client_create(modem, PN_GPDS);
	if (gd->client == NULL) {
		g_free(gd);
		return -ENOMEM;
	}

	ofono_gprs_set_data(gprs, gd);

	g_isi_client_set_timeout(gd->client, GPDS_TIMEOUT);
	g_isi_client_verify(gd->client, gpds_reachable_cb, gprs, NULL);

	return 0;
}

static void isi_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	ofono_gprs_set_data(gprs, NULL);

	if (gd == NULL)
		return;

	g_isi_client_destroy(gd->client);
	g_isi_client_destroy(gd->info_client);
	g_free(gd);
}

static void attach_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_gprs_cb_t cb = cbd->cb;
	const uint8_t *data = g_isi_msg_data(msg);

	if (g_isi_msg_error(msg) < 0) {
		DBG("ISI message error: %d", g_isi_msg_error(msg));
		goto error;
	}

	if (g_isi_msg_id(msg) != GPDS_ATTACH_RESP)
		return;

	if (g_isi_msg_data_len(msg) < 2)
		goto error;

	if (data[0] != GPDS_OK) {
		DBG("attach failed: %s", gpds_status_name(data[0]));
		goto error;
	}

	set_attach_mode(cbd->user, TRUE);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void detach_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_gprs_cb_t cb = cbd->cb;
	const uint8_t *data = g_isi_msg_data(msg);

	if (g_isi_msg_error(msg) < 0) {
		DBG("ISI client error: %d", g_isi_msg_error(msg));
		goto error;
	}

	if (g_isi_msg_id(msg) != GPDS_DETACH_RESP)
		return;

	if (g_isi_msg_data_len(msg) < 2)
		goto error;

	if (data[0] != GPDS_OK) {
		DBG("detach failed: %s", gpds_status_name(data[0]));
		goto error;
	}

	set_attach_mode(cbd->user, FALSE);

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, cbd->data);
}

static void isi_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct isi_cb_data *cbd = isi_cb_data_new(gprs, cb, data);

	if (cbd == NULL || gd == NULL)
		goto error;

	if (attached) {
		const unsigned char msg[] = {
			GPDS_ATTACH_REQ,
			GPDS_FOLLOW_OFF
		};

		if (g_isi_client_send_with_timeout(gd->client,
				msg, sizeof(msg),
				GPDS_ATTACH_TIMEOUT, attach_resp_cb,
				cbd, g_free))
			return;
	} else {
		const unsigned char msg[] = {
			GPDS_DETACH_REQ,
			0x00, /* filler */
			0x00  /* sub-blocks */
		};

		if (g_isi_client_send_with_timeout(gd->client,
				msg, sizeof(msg),
				GPDS_DETACH_TIMEOUT, detach_resp_cb,
				cbd, g_free))
			return;
	}

error:
	CALLBACK_WITH_FAILURE(cb, data);
	g_free(cbd);
}

static void status_resp_cb(const GIsiMessage *msg, void *opaque)
{
	struct isi_cb_data *cbd = opaque;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_gprs *gprs = cbd->data;
	int status;
	const uint8_t *data = g_isi_msg_data(msg);

	if (g_isi_msg_error(msg) < 0) {
		DBG("ISI message error: %d", g_isi_msg_error(msg));
		goto error;
	}

	if (g_isi_msg_id(msg) != GPDS_STATUS_RESP)
		return;

	if (g_isi_msg_data_len(msg) < 12)
		goto error;

	/* FIXME: the core still expects reg status, and not a boolean
	 * attached status here.*/
	switch (data[0]) {
	case GPDS_ATTACHED:
		status = GPRS_STAT_REGISTERED;
		break;
	case GPDS_DETACHED:
		status = GPRS_STAT_NOT_REGISTERED;
		break;
	default:
		status = GPRS_STAT_UNKNOWN;
	}

	suspend_notify(gprs, data[10], data[11]);

	CALLBACK_WITH_SUCCESS(cb, status, cbd->data);
	return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
}

static void isi_gprs_attached_status(struct ofono_gprs *gprs,
						ofono_gprs_status_cb_t cb,
						void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct isi_cb_data *cbd = isi_cb_data_new(NULL, cb, data);

	const unsigned char msg[] = {
		GPDS_STATUS_REQ,
	};

	if (cbd == NULL || gd == NULL)
		goto error;

	if (g_isi_client_send(gd->client, msg, sizeof(msg),
				status_resp_cb, cbd, g_free))
		return;

error:
	CALLBACK_WITH_FAILURE(cb, -1, data);
	g_free(cbd);
}

static struct ofono_gprs_driver driver = {
	.name			= "isimodem",
	.probe			= isi_gprs_probe,
	.remove			= isi_gprs_remove,
	.set_attached		= isi_gprs_set_attached,
	.attached_status	= isi_gprs_attached_status,
};

void isi_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void isi_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
