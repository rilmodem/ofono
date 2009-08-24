/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Aki Niemi <aki.niemi@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#define DECLARE_FAILURE(e) 			\
	struct ofono_error e;			\
	e.type = OFONO_ERROR_TYPE_FAILURE;	\
	e.error = 0				\

#define DECLARE_SUCCESS(e) 			\
	struct ofono_error e;			\
	e.type = OFONO_ERROR_TYPE_NO_ERROR;	\
	e.error = 0				\

struct isi_cb_data {
	void *cb;
	void *data;
	struct ofono_modem *modem;
	void *user;
};

struct isi_version {
	unsigned short major;
	unsigned short minor;
};

static inline struct isi_cb_data *isi_cb_data_new(struct ofono_modem *modem,
						void *cb, void *data)
{
	struct isi_cb_data *ret;

	ret = g_try_new0(struct isi_cb_data, 1);

	if (ret) {
		ret->cb = cb;
		ret->data = data;
		ret->modem = modem;
	}
	return ret;
}

static inline void dump_msg(const unsigned char *msg, size_t len)
{
	char dumpstr[len * 5 + len / 10 + 1];
	size_t i;

	for (i = 0; i < len; i++)
		sprintf(dumpstr + i * 5, "0x%02x%s",
			msg[i], (i + 1) % 10 == 0 ? "\n" : " ");

	DBG("\n%s\n", dumpstr);
}

extern void isi_phonebook_init();
extern void isi_phonebook_exit();

extern void isi_devinfo_init();
extern void isi_devinfo_exit();

extern void isi_netreg_init();
extern void isi_netreg_exit();

extern void isi_voicecall_init();
extern void isi_voicecall_exit();

extern void isi_sms_init();
extern void isi_sms_exit();

extern void isi_sim_init();
extern void isi_sim_exit();

extern void isi_ussd_init();
extern void isi_ussd_exit();

extern void isi_ssn_init();
extern void isi_ssn_exit();

extern void isi_call_forwarding_init();
extern void isi_call_forwarding_exit();

extern void isi_call_settings_init();
extern void isi_call_settings_exit();

extern void isi_call_barring_init();
extern void isi_call_barring_exit();

extern void isi_call_meter_init();
extern void isi_call_meter_exit();
