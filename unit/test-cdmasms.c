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

#include <string.h>

#include <glib.h>

#include "cdma-smsutil.h"

static inline void check_text(const char *decoded, const char *expected)
{
	if (expected == NULL) {
		g_assert(decoded == NULL);
		return;
	}

	g_assert(decoded != NULL);
	g_assert(g_str_equal(decoded, expected));
}

struct wmt_deliver_test {
	const guint8 *tpdu;
	guint8 tpdu_len;
	const char *text;
	const char *oaddr;
};

guint8 wmt_deliver_1[] = { 0x00, 0x00, 0x02, 0x10, 0x02, 0x02, 0x05, 0x01,
				0xC4, 0x8D, 0x15, 0x9C, 0x08, 0x0D, 0x00,
				0x03, 0x1B, 0xEE, 0xF0, 0x01, 0x06, 0x10,
				0x2C, 0x8C, 0xBB, 0x36, 0x6F };

guint8 wmt_deliver_2[] = { 0x00, 0x00, 0x02, 0x10, 0x02, 0x02, 0x07, 0x02,
				0xA1, 0x62, 0x51, 0x55, 0xA6, 0x40, 0x08,
				0x18, 0x00, 0x03, 0x10, 0x00, 0x40, 0x01,
				0x06, 0x10, 0x25, 0x4C, 0xBC, 0xFA, 0x00,
				0x03, 0x06, 0x03, 0x08, 0x20, 0x13, 0x43,
				0x12, 0x0D, 0x01, 0x01 };

static struct wmt_deliver_test wmt_deliver_data_1 = {
	.tpdu = wmt_deliver_1,
	.tpdu_len = sizeof(wmt_deliver_1),
	.text = "Hello",
	.oaddr = "1234567"
};

static struct wmt_deliver_test wmt_deliver_data_2 = {
	.tpdu = wmt_deliver_2,
	.tpdu_len = sizeof(wmt_deliver_2),
	.text = "Test",
	.oaddr = "8589455699"
};

static void test_wmt_deliver(gconstpointer data)
{
	const struct wmt_deliver_test *test = data;
	gboolean ret;
	struct cdma_sms s;
	const char *addr;
	char *message;

	memset(&s, 0, sizeof(struct cdma_sms));

	ret = cdma_sms_decode(test->tpdu, test->tpdu_len, &s);

	g_assert(ret == TRUE);

	g_assert(s.type == CDMA_SMS_TP_MSG_TYPE_P2P);

	g_assert(s.p2p_msg.teleservice_id == CDMA_SMS_TELESERVICE_ID_WMT);

	addr = cdma_sms_address_to_string(&s.p2p_msg.oaddr);
	check_text(addr, test->oaddr);

	message = cdma_sms_decode_text(&s.p2p_msg.bd.wmt_deliver.ud);
	check_text(message, test->text);

	g_free(message);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_data_func("/test-cdmasms/WMT DELIVER 1",
			&wmt_deliver_data_1, test_wmt_deliver);

	g_test_add_data_func("/test-cdmasms/WMT DELIVER 2",
			&wmt_deliver_data_2, test_wmt_deliver);

	return g_test_run();
}
