/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2010  ST-Ericsson AB.
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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/gprs.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"
#include "vendor.h"

static const char *cgreg_prefix[] = { "+CGREG:", NULL };
static const char *cgdcont_prefix[] = { "+CGDCONT:", NULL };
static const char *none_prefix[] = { NULL };

struct gprs_data {
	GAtChat *chat;
	unsigned int vendor;
};

static void at_cgatt_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_cb_t cb = cbd->cb;
	struct ofono_error error;

	decode_at_error(&error, g_at_result_final_response(result));

	cb(&error, cbd->data);
}

static void at_gprs_set_attached(struct ofono_gprs *gprs, int attached,
					ofono_gprs_cb_t cb, void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);
	char buf[64];

	snprintf(buf, sizeof(buf), "AT+CGATT=%i", attached ? 1 : 0);

	if (g_at_chat_send(gd->chat, buf, none_prefix,
				at_cgatt_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, data);
}

static void at_cgreg_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	ofono_gprs_status_cb_t cb = cbd->cb;
	struct ofono_error error;
	int status;
	struct gprs_data *gd = cbd->user;

	decode_at_error(&error, g_at_result_final_response(result));

	if (!ok) {
		cb(&error, -1, cbd->data);
		return;
	}

	if (at_util_parse_reg(result, "+CGREG:", NULL, &status,
				NULL, NULL, NULL, gd->vendor) == FALSE) {
		CALLBACK_WITH_FAILURE(cb, -1, cbd->data);
		return;
	}

	cb(&error, status, cbd->data);
}

static void at_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb,
					void *data)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	struct cb_data *cbd = cb_data_new(cb, data);

	cbd->user = gd;

	switch (gd->vendor) {
	case OFONO_VENDOR_GOBI:
		/*
		 * Send *CNTI=0 to find out the current tech, it will be
		 * intercepted in gobi_cnti_notify in network registration
		 */
		g_at_chat_send(gd->chat, "AT*CNTI=0", none_prefix,
				NULL, NULL, NULL);
		break;
	case OFONO_VENDOR_NOVATEL:
		/*
		 * Send $CNTI=0 to find out the current tech, it will be
		 * intercepted in nw_cnti_notify in network registration
		 */
		g_at_chat_send(gd->chat, "AT$CNTI=0", none_prefix,
				NULL, NULL, NULL);
		break;
	}

	if (g_at_chat_send(gd->chat, "AT+CGREG?", cgreg_prefix,
				at_cgreg_cb, cbd, g_free) > 0)
		return;

	g_free(cbd);

	CALLBACK_WITH_FAILURE(cb, -1, data);
}

static void cgreg_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	int status;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	if (at_util_parse_reg_unsolicited(result, "+CGREG:", &status,
				NULL, NULL, NULL, gd->vendor) == FALSE)
		return;

	ofono_gprs_status_notify(gprs, status);
}

static void cgev_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	const char *event;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CGEV:"))
		return;

	if (!g_at_result_iter_next_unquoted_string(&iter, &event))
		return;

	if (g_str_equal(event, "NW DETACH") ||
			g_str_equal(event, "ME DETACH")) {
		ofono_gprs_detached_notify(gprs);
		return;
	}
}

static void xdatastat_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	int stat;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+XDATASTAT:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &stat))

	DBG("stat %d", stat);

	switch (stat) {
	case 0:
		ofono_gprs_suspend_notify(gprs, GPRS_SUSPENDED_UNKNOWN_CAUSE);
		break;
	case 1:
		ofono_gprs_resume_notify(gprs);
		break;
	}
}

static void huawei_mode_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	int mode, submode;
	gint bearer;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^MODE:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &mode))
		return;

	if (!g_at_result_iter_next_number(&iter, &submode))
		return;

	switch (submode) {
	case 1:
	case 2:
		bearer = 1;	/* GPRS */
		break;
	case 3:
		bearer = 2;	/* EDGE */
		break;
	case 4:
		bearer = 3;	/* UMTS */
		break;
	case 5:
		bearer = 5;	/* HSDPA */
		break;
	case 6:
		bearer = 4;	/* HSUPA */
		break;
	case 7:
	case 9:
		bearer = 6;	/* HSUPA + HSDPA */
		break;
	default:
		bearer = 0;
		break;
	}

	ofono_gprs_bearer_notify(gprs, bearer);
}

static void telit_mode_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	gint nt, bearer;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "#PSNT:"))
		return;

	if (!g_at_result_iter_next_number(&iter,&nt))
		return;

	switch (nt) {
	case 0:
		bearer = 1;    /* GPRS */
		break;
	case 1:
		bearer = 2;    /* EDGE */
		break;
	case 2:
		bearer = 3;    /* UMTS */
		break;
	case 3:
		bearer = 5;    /* HSDPA */
		break;
	default:
		bearer = 0;
		break;
	}

	ofono_gprs_bearer_notify(gprs, bearer);
}

static void cpsb_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	GAtResultIter iter;
	gint bearer;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CPSB:"))
		return;

	if (!g_at_result_iter_next_number(&iter, NULL))
		return;

	if (!g_at_result_iter_next_number(&iter, &bearer))
		return;

	ofono_gprs_bearer_notify(gprs, bearer);
}

static void gprs_initialized(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	g_at_chat_register(gd->chat, "+CGEV:", cgev_notify, FALSE, gprs, NULL);
	g_at_chat_register(gd->chat, "+CGREG:", cgreg_notify,
						FALSE, gprs, NULL);

	switch (gd->vendor) {
	case OFONO_VENDOR_HUAWEI:
		g_at_chat_register(gd->chat, "^MODE:", huawei_mode_notify,
						FALSE, gprs, NULL);
		break;
	case OFONO_VENDOR_TELIT:
		g_at_chat_register(gd->chat, "#PSNT:", telit_mode_notify,
						FALSE, gprs, NULL);
		g_at_chat_send(gd->chat, "AT#PSNT=1", none_prefix,
						NULL, NULL, NULL);
	default:
		g_at_chat_register(gd->chat, "+CPSB:", cpsb_notify,
						FALSE, gprs, NULL);
		g_at_chat_send(gd->chat, "AT+CPSB=1", none_prefix,
						NULL, NULL, NULL);
		break;
	}

	switch (gd->vendor) {
	case OFONO_VENDOR_IFX:
		/* Register for GPRS suspend notifications */
		g_at_chat_register(gd->chat, "+XDATASTAT:", xdatastat_notify,
						FALSE, gprs, NULL);
		g_at_chat_send(gd->chat, "AT+XDATASTAT=1", none_prefix,
						NULL, NULL, NULL);
		break;
	}

	ofono_gprs_register(gprs);
}

static void at_cgreg_test_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	gint range[2];
	GAtResultIter iter;
	int cgreg1 = 0;
	int cgreg2 = 0;
	const char *cmd;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

retry:
	if (!g_at_result_iter_next(&iter, "+CGREG:"))
		goto error;

	if (!g_at_result_iter_open_list(&iter))
		goto retry;

	while (g_at_result_iter_next_range(&iter, &range[0], &range[1])) {
		if (1 >= range[0] && 1 <= range[1])
			cgreg1 = 1;
		if (2 >= range[0] && 2 <= range[1])
			cgreg2 = 1;
	}

	g_at_result_iter_close_list(&iter);

	if (cgreg2)
		cmd = "AT+CGREG=2";
	else if (cgreg1)
		cmd = "AT+CGREG=1";
	else
		goto error;

	g_at_chat_send(gd->chat, cmd, none_prefix, NULL, NULL, NULL);
	g_at_chat_send(gd->chat, "AT+CGAUTO=0", none_prefix, NULL, NULL, NULL);

	switch (gd->vendor) {
	case OFONO_VENDOR_MBM:
		/* Ericsson MBM and ST-E modems don't support AT+CGEREP=2,1 */
		g_at_chat_send(gd->chat, "AT+CGEREP=1,0", none_prefix,
			gprs_initialized, gprs, NULL);
		break;
	case OFONO_VENDOR_NOKIA:
		/* Nokia data cards don't support AT+CGEREP=1,0 either */
		g_at_chat_send(gd->chat, "AT+CGEREP=1", none_prefix,
			gprs_initialized, gprs, NULL);
		break;
	default:
		g_at_chat_send(gd->chat, "AT+CGEREP=2,1", none_prefix,
			gprs_initialized, gprs, NULL);
		break;
	}

	return;

error:
	ofono_info("GPRS not supported on this device");
	ofono_gprs_remove(gprs);
}

static void at_cgdcont_test_cb(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_gprs *gprs = user_data;
	struct gprs_data *gd = ofono_gprs_get_data(gprs);
	GAtResultIter iter;
	int min, max;
	const char *pdp_type;
	gboolean found = FALSE;

	if (!ok)
		goto error;

	g_at_result_iter_init(&iter, result);

	while (!found && g_at_result_iter_next(&iter, "+CGDCONT:")) {
		gboolean in_list = FALSE;

		if (!g_at_result_iter_open_list(&iter))
			continue;

		if (g_at_result_iter_next_range(&iter, &min, &max) == FALSE)
			continue;

		if (!g_at_result_iter_close_list(&iter))
			continue;

		if (g_at_result_iter_open_list(&iter))
			in_list = TRUE;

		if (!g_at_result_iter_next_string(&iter, &pdp_type))
			continue;

		if (in_list && !g_at_result_iter_close_list(&iter))
			continue;

		/* We look for IP PDPs */
		if (g_str_equal(pdp_type, "IP"))
			found = TRUE;
	}

	if (found == FALSE)
		goto error;

	ofono_gprs_set_cid_range(gprs, min, max);

	g_at_chat_send(gd->chat, "AT+CGREG=?", cgreg_prefix,
			at_cgreg_test_cb, gprs, NULL);

	return;

error:
	ofono_info("GPRS not supported on this device");
	ofono_gprs_remove(gprs);
}

static int at_gprs_probe(struct ofono_gprs *gprs,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct gprs_data *gd;

	gd = g_try_new0(struct gprs_data, 1);
	if (gd == NULL)
		return -ENOMEM;

	gd->chat = g_at_chat_clone(chat);
	gd->vendor = vendor;

	ofono_gprs_set_data(gprs, gd);

	g_at_chat_send(gd->chat, "AT+CGDCONT=?", cgdcont_prefix,
			at_cgdcont_test_cb, gprs, NULL);

	return 0;
}

static void at_gprs_remove(struct ofono_gprs *gprs)
{
	struct gprs_data *gd = ofono_gprs_get_data(gprs);

	ofono_gprs_set_data(gprs, NULL);

	g_at_chat_unref(gd->chat);
	g_free(gd);
}

static struct ofono_gprs_driver driver = {
	.name			= "atmodem",
	.probe			= at_gprs_probe,
	.remove			= at_gprs_remove,
	.set_attached		= at_gprs_set_attached,
	.attached_status	= at_gprs_registration_status,
};

void at_gprs_init(void)
{
	ofono_gprs_driver_register(&driver);
}

void at_gprs_exit(void)
{
	ofono_gprs_driver_unregister(&driver);
}
