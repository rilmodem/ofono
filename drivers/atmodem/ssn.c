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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/ssn.h>

#include "gatchat.h"
#include "gatresult.h"

#include "atmodem.h"

static const char *none_prefix[] = { NULL };

static void cssi_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_ssn *ssn = user_data;
	GAtResultIter iter;
	int code1, index;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSI:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code1))
		return;

	if (!g_at_result_iter_next_number(&iter, &index))
		index = 0;

	ofono_ssn_cssi_notify(ssn, code1, index);
}

static void cssu_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_ssn *ssn = user_data;
	GAtResultIter iter;
	int code2;
	int index = -1;
	const char *num;
	struct ofono_phone_number ph;

	ph.number[0] = '\0';
	ph.type = 129;

	g_at_result_iter_init(&iter, result);

	if (!g_at_result_iter_next(&iter, "+CSSU:"))
		return;

	if (!g_at_result_iter_next_number(&iter, &code2))
		return;

	/* This field is optional, if we can't read it, try to skip it */
	if (!g_at_result_iter_next_number(&iter, &index) &&
			!g_at_result_iter_skip_next(&iter))
		goto out;

	if (!g_at_result_iter_next_string(&iter, &num))
		goto out;

	strncpy(ph.number, num, OFONO_MAX_PHONE_NUMBER_LENGTH);

	if (!g_at_result_iter_next_number(&iter, &ph.type))
		return;

out:
	ofono_ssn_cssu_notify(ssn, code2, index, &ph);
}

static void at_ssn_initialized(gboolean ok, GAtResult *result,
				gpointer user_data)
{
	struct ofono_ssn *ssn = user_data;
	GAtChat *chat = ofono_ssn_get_data(ssn);

	g_at_chat_register(chat, "+CSSI:", cssi_notify, FALSE, ssn, NULL);
	g_at_chat_register(chat, "+CSSU:", cssu_notify, FALSE, ssn, NULL);

	ofono_ssn_register(ssn);
}

static int at_ssn_probe(struct ofono_ssn *ssn, unsigned int vendor,
				void *data)
{
	GAtChat *chat = data;

	chat = g_at_chat_clone(chat);

	ofono_ssn_set_data(ssn, chat);
	g_at_chat_send(chat, "AT+CSSN=1,1", none_prefix,
			at_ssn_initialized, ssn, NULL);

	return 0;
}

static void at_ssn_remove(struct ofono_ssn *ssn)
{
	GAtChat *chat = ofono_ssn_get_data(ssn);

	g_at_chat_unref(chat);
	ofono_ssn_set_data(ssn, NULL);
}

static struct ofono_ssn_driver driver = {
	.name = "atmodem",
	.probe = at_ssn_probe,
	.remove = at_ssn_remove,
};

void at_ssn_init(void)
{
	ofono_ssn_driver_register(&driver);
}

void at_ssn_exit(void)
{
	ofono_ssn_driver_unregister(&driver);
}
