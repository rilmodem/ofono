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

#define _GNU_SOURCE
#include <glib.h>
#include <errno.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/cdma-netreg.h>

#include "gatchat.h"

#include "huaweimodem.h"

static const char *sysinfo_prefix[] = { "^SYSINFO:", NULL };

static gboolean parse_sysinfo(GAtResult *result, gint *status)
{
	GAtResultIter iter;
	gint srv_status;
	gint srv_domain;
	gint roaming_status;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^SYSINFO:"))
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, &srv_status))
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, &srv_domain))
		return FALSE;

	if (!g_at_result_iter_next_number(&iter, &roaming_status))
		return FALSE;

	DBG("%d, %d, %d", srv_status, srv_domain, roaming_status);

	switch (srv_status) {
	case 1:	/* Restricted service */
	case 2:	/* Service valid */
	case 3:	/* Restricted region service */
		if (roaming_status)
			*status = CDMA_NETWORK_REGISTRATION_STATUS_ROAMING;
		else
			*status = CDMA_NETWORK_REGISTRATION_STATUS_REGISTERED;
		break;
	case 0:	/* No service */
	case 4:	/* Not registered */
	default:
		*status = CDMA_NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;
		break;
	}

	switch (srv_domain) {
	case 0: /* No service */
		*status = CDMA_NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;
		break;
	case 1: /* Only CS */
	case 2: /* Only PS */
	case 3: /* CS  PS */
	case 4: /* CS registered, PS in searching state */
	case 255: /* CDMA not supported */
		break;
	}

	return TRUE;
}

static void sysinfo_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_cdma_netreg *netreg = user_data;
	int status;

	if (!ok)
		return;

	if (parse_sysinfo(result, &status) == FALSE) {
		ofono_error("Invalid SYSINFO values");
		return;
	}

	ofono_cdma_netreg_status_notify(netreg, status);
}

static void mode_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_cdma_netreg *netreg = user_data;
	GAtChat *chat = ofono_cdma_netreg_get_data(netreg);

	g_at_chat_send(chat, "AT^SYSINFO", sysinfo_prefix,
				sysinfo_cb, netreg, NULL);
}

static void rssilvl_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_cdma_netreg *netreg = user_data;
	int strength;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^RSSILVL:"))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &strength))
		goto error;

	if (strength == 99)
		strength = 100;

	ofono_cdma_netreg_strength_notify(netreg, strength);

	return;

error:
	ofono_error("Invalid RSSILVL value");
}

static void hrssilvl_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_cdma_netreg *netreg = user_data;
	int strength;
	GAtResultIter iter;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "^HRSSILVL:"))
		goto error;

	if (!g_at_result_iter_next_number(&iter, &strength))
		goto error;

	if (strength == 99)
		strength = 100;

	ofono_cdma_netreg_data_strength_notify(netreg, strength);

	return;

error:
	ofono_error("Invalid HRSSILVL value");
}

static void probe_cb(gboolean ok, GAtResult *result, gpointer user_data)
{
	struct ofono_cdma_netreg *netreg = user_data;
	GAtChat *chat = ofono_cdma_netreg_get_data(netreg);

	if (!ok) {
		ofono_cdma_netreg_remove(netreg);
		return;
	}

	g_at_chat_register(chat, "^MODE:",
				mode_notify, FALSE, netreg, NULL);

	g_at_chat_register(chat, "^RSSILVL:",
				rssilvl_notify, FALSE, netreg, NULL);

	g_at_chat_register(chat, "^HRSSILVL:",
				hrssilvl_notify, FALSE, netreg, NULL);

	ofono_cdma_netreg_register(netreg);
}

static int huawei_cdma_netreg_probe(struct ofono_cdma_netreg *netreg,
				unsigned int vendor, void *data)
{
	GAtChat *chat = g_at_chat_clone(data);

	ofono_cdma_netreg_set_data(netreg, chat);

	g_at_chat_send(chat, "AT^SYSINFO", sysinfo_prefix,
				probe_cb, netreg, NULL);

	return 0;
}

static void huawei_cdma_netreg_remove(struct ofono_cdma_netreg *netreg)
{
	GAtChat *chat = ofono_cdma_netreg_get_data(netreg);

	ofono_cdma_netreg_set_data(netreg, NULL);

	g_at_chat_unref(chat);
}

static struct ofono_cdma_netreg_driver driver = {
	.name	= "huaweimodem",
	.probe	= huawei_cdma_netreg_probe,
	.remove	= huawei_cdma_netreg_remove,
};

void huawei_cdma_netreg_init(void)
{
	ofono_cdma_netreg_driver_register(&driver);
}

void huawei_cdma_netreg_exit(void)
{
	ofono_cdma_netreg_driver_unregister(&driver);
}
