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

struct isi_data {
	struct ofono_modem *modem;
	struct netreg_data *netreg;
};

struct isi_cb_data {
	void *cb;
	void *data;
	struct ofono_modem *modem;
	void *user;
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

void dump_msg(const unsigned char *msg, size_t len);
