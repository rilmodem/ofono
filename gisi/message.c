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

#include <stdint.h>
#include <errno.h>
#include <glib.h>

#include "message.h"

int g_isi_msg_version_major(const GIsiMessage *msg)
{
	return msg && msg->version ? msg->version->major : -1;
}

int g_isi_msg_version_minor(const GIsiMessage *msg)
{
	return msg && msg->version ? msg->version->minor : -1;
}

int g_isi_msg_error(const GIsiMessage *msg)
{
	return msg ? -msg->error : -EINVAL;
}

uint8_t g_isi_msg_resource(const GIsiMessage *msg)
{
	return msg && msg->addr ? msg->addr->spn_resource : 0;
}

uint16_t g_isi_msg_object(const GIsiMessage *msg)
{
	return msg && msg->addr ? (msg->addr->spn_dev << 8) | msg->addr->spn_obj : 0;
}

uint8_t g_isi_msg_id(const GIsiMessage *msg)
{
	const uint8_t *buf;

	if (!msg || !msg->data || msg->len < 2)
		return 0;

	buf = msg->data;

	return buf[1];
}

uint8_t g_isi_msg_utid(const GIsiMessage *msg)
{
	const uint8_t *buf;

	if (!msg || !msg->data || msg->len < 2)
		return 0;

	buf = msg->data;

	return buf[0];
}

size_t g_isi_msg_data_len(const GIsiMessage *msg)
{
	return msg && msg->data ? msg->len - 2 : 0;
}

const void *g_isi_msg_data(const GIsiMessage *msg)
{
	return msg && msg->data ? (void *)msg->data + 2 : NULL;
}

gboolean g_isi_msg_data_get_byte(const GIsiMessage *msg, unsigned offset,
					uint8_t *byte)
{
	const uint8_t *buf = g_isi_msg_data(msg);

	if (!buf || g_isi_msg_data_len(msg) < offset)
		return FALSE;

	if (byte)
		*byte = buf[offset];

	return TRUE;
}

gboolean g_isi_msg_data_get_struct(const GIsiMessage *msg, unsigned offset,
					const void **type, size_t len)
{
	if (g_isi_msg_data_len(msg) < offset + len)
		return FALSE;

	if (type)
		*type = g_isi_msg_data(msg) + offset;

	return TRUE;
}
