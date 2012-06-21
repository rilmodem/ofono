/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#include <glib.h>

struct cb_data {
	void *cb;
	void *data;
	void *user;
};

static inline struct cb_data *cb_data_new(void *cb, void *data)
{
	struct cb_data *ret;

	ret = g_new0(struct cb_data, 1);
	ret->cb = cb;
	ret->data = data;
	ret->user = NULL;

	return ret;
}

#define CALLBACK_WITH_FAILURE(cb, args...)		\
	do {						\
		struct ofono_error cb_e;		\
		cb_e.type = OFONO_ERROR_TYPE_FAILURE;	\
		cb_e.error = 0;				\
							\
		cb(&cb_e, ##args);			\
	} while (0)					\

#define CALLBACK_WITH_SUCCESS(f, args...)		\
	do {						\
		struct ofono_error e;			\
		e.type = OFONO_ERROR_TYPE_NO_ERROR;	\
		e.error = 0;				\
		f(&e, ##args);				\
	} while (0)
