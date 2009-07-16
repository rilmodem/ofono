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

struct at_data {
	GAtChat *parser;
	struct ofono_modem *modem;
	GIOChannel *io;
	char *driver;

	struct netreg_data *netreg;
	struct voicecall_data *voicecall;
	struct sms_data *sms;
	struct pb_data *pb;
};

void decode_at_error(struct ofono_error *error, const char *final);
void dump_response(const char *func, gboolean ok, GAtResult *result);

struct cb_data {
	void *cb;
	void *data;
	struct ofono_modem *modem;
	void *user;
};

static inline struct cb_data *cb_data_new(struct ofono_modem *modem,
						void *cb, void *data)
{
	struct cb_data *ret;

	ret = g_try_new0(struct cb_data, 1);

	if (!ret)
		return ret;

	ret->cb = cb;
	ret->data = data;
	ret->modem = modem;

	return ret;
}

#define DECLARE_FAILURE(e) 			\
	struct ofono_error e;			\
	e.type = OFONO_ERROR_TYPE_FAILURE;	\
	e.error = 0				\

extern struct ofono_error g_ok;

extern void at_network_registration_init(struct ofono_modem *modem);
extern void at_network_registration_exit(struct ofono_modem *modem);

extern void at_call_forwarding_init(struct ofono_modem *modem);
extern void at_call_forwarding_exit(struct ofono_modem *modem);

extern void at_call_settings_init(struct ofono_modem *modem);
extern void at_call_settings_exit(struct ofono_modem *modem);

extern void at_ussd_init(struct ofono_modem *modem);
extern void at_ussd_exit(struct ofono_modem *modem);

extern void at_voicecall_init(struct ofono_modem *modem);
extern void at_voicecall_exit(struct ofono_modem *modem);

extern void at_call_meter_init(struct ofono_modem *modem);
extern void at_call_meter_exit(struct ofono_modem *modem);

extern void at_call_barring_init(struct ofono_modem *modem);
extern void at_call_barring_exit(struct ofono_modem *modem);

extern void at_sim_init(struct ofono_modem *modem);
extern void at_sim_exit(struct ofono_modem *modem);

extern void at_sms_init(struct ofono_modem *modem);
extern void at_sms_exit(struct ofono_modem *modem);

extern void at_phonebook_init(struct ofono_modem *modem);
extern void at_phonebook_exit(struct ofono_modem *modem);
