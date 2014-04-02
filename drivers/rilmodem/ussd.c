/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2013 Jolla Ltd
 *  Copyright (C) 2013 Canonical Ltd
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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/ussd.h>
#include <smsutil.h>
#include <util.h>

#include "gril.h"
#include "grilutil.h"
#include "grilrequest.h"
#include "grilunsol.h"

#include "rilmodem.h"

#include "ril_constants.h"

struct ussd_data {
	GRil *ril;
};

static gboolean request_success(gpointer data)
{
	struct cb_data *cbd = data;
	ofono_ussd_cb_t cb = cbd->cb;

	CALLBACK_WITH_SUCCESS(cb, cbd->data);
	g_free(cbd);

	return FALSE;
}

static void ril_ussd_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_ussd *ussd = user_data;
	struct ussd_data *ud = ofono_ussd_get_data(ussd);

	/*
	 * We fake an ON_USSD event if there was an error sending the request,
	 * as core will be waiting for one to respond to the Initiate() call.
	 * Note that we already made the callback (see ril_ussd_request()).
	 */
	if (message->error == RIL_E_SUCCESS)
		g_ril_print_response_no_args(ud->ril, message);
	else
		ofono_ussd_notify(ussd, OFONO_USSD_STATUS_NOT_SUPPORTED,
					0, NULL, 0);
}

static void ril_ussd_request(struct ofono_ussd *ussd, int dcs,
			const unsigned char *pdu, int len,
			ofono_ussd_cb_t cb, void *data)
{
	struct ussd_data *ud = ofono_ussd_get_data(ussd);
	struct cb_data *cbd = cb_data_new(cb, data, ussd);
	enum sms_charset charset;
	int ret = 0;

	if (cbs_dcs_decode(dcs, NULL, NULL, &charset, NULL, NULL, NULL)) {
		/* TODO: send other alphabets (maybe needed by STK) */
		if (charset == SMS_CHARSET_7BIT) {
			unsigned char *unpacked_buf;
			long written;

			unpacked_buf = unpack_7bit(pdu, len, 0, TRUE, 0,
							&written, 1);

			if (written >= 1) {
				struct parcel rilp;

				*(unpacked_buf + written) = '\0';
				g_ril_request_send_ussd(ud->ril,
							(char *) unpacked_buf,
							&rilp);

				ret = g_ril_send(ud->ril, RIL_REQUEST_SEND_USSD,
							&rilp, ril_ussd_cb,
							ussd, NULL);
			}

			g_free(unpacked_buf);
		} else {
			ofono_error("%s: No support for charset %d",
					__func__, charset);
		}
	}

	/*
	 * We do not wait for the SEND_USSD reply to do the callback, as some
	 * networks send it after sending one or more ON_USSD events. From the
	 * ofono core perspective, Initiate() does not return until one ON_USSD
	 * event is received: making here a successful callback just makes the
	 * core wait for that event.
	 */
	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, data);
	} else {
		g_idle_add(request_success, cbd);
	}
}
static void ril_ussd_cancel_cb(struct ril_msg *message, gpointer user_data)
{
	struct cb_data *cbd = user_data;
	struct ofono_ussd *ussd = cbd->user;
	struct ussd_data *ud = ofono_ussd_get_data(ussd);
	ofono_ussd_cb_t cb = cbd->cb;

	if (message->error == RIL_E_SUCCESS) {
		g_ril_print_response_no_args(ud->ril, message);
		CALLBACK_WITH_SUCCESS(cb, cbd->data);
	} else {
		CALLBACK_WITH_FAILURE(cb, cbd->data);
	}
}

static void ril_ussd_cancel(struct ofono_ussd *ussd,
				ofono_ussd_cb_t cb, void *user_data)
{
	struct ussd_data *ud = ofono_ussd_get_data(ussd);
	struct cb_data *cbd = cb_data_new(cb, user_data, ussd);
	int ret;

	ret = g_ril_send(ud->ril, RIL_REQUEST_CANCEL_USSD, NULL,
				ril_ussd_cancel_cb, cbd, g_free);

	if (ret <= 0) {
		g_free(cbd);
		CALLBACK_WITH_FAILURE(cb, user_data);
	}
}

static void ril_ussd_notify(struct ril_msg *message, gpointer user_data)
{
	struct ofono_ussd *ussd = user_data;
	struct ussd_data *ud = ofono_ussd_get_data(ussd);
	struct unsol_ussd *unsol;

	unsol = g_ril_unsol_parse_ussd(ud->ril, message);
	if (unsol == NULL) {
		ofono_error("%s: Parsing error", __func__);
		return;
	}

	/*
	 * TODO
	 *
	 * With data coding scheme 0x44, we are saying that the ussd string is
	 * 8-bit data, uncompressed, and with unspecified message class. This
	 * must be changed in the future so we get the UTF16 from the RIL parcel
	 * and decode it properly. For the DCS coding, see 3gpp 23.038, sect. 5.
	 */
	if (unsol->message != NULL)
		ofono_ussd_notify(ussd, unsol->type, 0x44,
					(const unsigned char *) unsol->message,
					strlen(unsol->message));
	else
		ofono_ussd_notify(ussd, unsol->type, 0, NULL, 0);

	g_ril_unsol_free_ussd(unsol);
}

static gboolean ril_delayed_register(gpointer user_data)
{
	struct ofono_ussd *ussd = user_data;
	struct ussd_data *ud = ofono_ussd_get_data(ussd);

	DBG("");

	ofono_ussd_register(ussd);

	/* Register for USSD responses */
	g_ril_register(ud->ril, RIL_UNSOL_ON_USSD, ril_ussd_notify, ussd);

	return FALSE;
}

static int ril_ussd_probe(struct ofono_ussd *ussd,
					unsigned int vendor,
					void *user)
{
	GRil *ril = user;
	struct ussd_data *ud = g_new0(struct ussd_data, 1);

	ud->ril = g_ril_clone(ril);
	ofono_ussd_set_data(ussd, ud);
	g_idle_add(ril_delayed_register, ussd);

	return 0;
}

static void ril_ussd_remove(struct ofono_ussd *ussd)
{
	struct ussd_data *ud = ofono_ussd_get_data(ussd);
	ofono_ussd_set_data(ussd, NULL);

	g_ril_unref(ud->ril);
	g_free(ud);
}

static struct ofono_ussd_driver driver = {
	.name		= RILMODEM,
	.probe		= ril_ussd_probe,
	.remove		= ril_ussd_remove,
	.request	= ril_ussd_request,
	.cancel		= ril_ussd_cancel
};

void ril_ussd_init(void)
{
	ofono_ussd_driver_register(&driver);
}

void ril_ussd_exit(void)
{
	ofono_ussd_driver_unregister(&driver);
}
