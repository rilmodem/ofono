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
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <glib.h>

#include "message.h"

int g_isi_msg_version_major(const GIsiMessage *msg)
{
	if (msg == NULL || msg->version == NULL)
		return -1;

	return msg->version->major;
}

int g_isi_msg_version_minor(const GIsiMessage *msg)
{
	if (msg == NULL || msg->version == NULL)
		return -1;

	return msg->version->minor;
}

int g_isi_msg_error(const GIsiMessage *msg)
{
	return msg != NULL ? -msg->error : -EINVAL;
}

const char *g_isi_msg_strerror(const GIsiMessage *msg)
{
	return strerror(-g_isi_msg_error(msg));
}

uint8_t g_isi_msg_resource(const GIsiMessage *msg)
{
	if (msg == NULL || msg->addr == NULL)
		return 0;

	return msg->addr->spn_resource;
}

uint16_t g_isi_msg_object(const GIsiMessage *msg)
{
	if (msg == NULL || msg->addr == NULL)
		return 0;

	return (msg->addr->spn_dev << 8) | msg->addr->spn_obj;
}

uint8_t g_isi_msg_id(const GIsiMessage *msg)
{
	const uint8_t *buf;

	if (msg == NULL || msg->data == NULL || msg->len < 2)
		return 0;

	buf = msg->data;

	return buf[1];
}

uint8_t g_isi_msg_utid(const GIsiMessage *msg)
{
	const uint8_t *buf;

	if (msg == NULL || msg->data == NULL || msg->len < 2)
		return 0;

	buf = msg->data;

	return buf[0];
}

size_t g_isi_msg_data_len(const GIsiMessage *msg)
{
	if (msg == NULL || msg->data == NULL)
		return 0;

	return msg->len - 2;
}

const void *g_isi_msg_data(const GIsiMessage *msg)
{
	if (msg == NULL || msg->data == NULL)
		return NULL;

	return (void *)msg->data + 2;
}

gboolean g_isi_msg_data_get_byte(const GIsiMessage *msg, unsigned offset,
					uint8_t *byte)
{
	const uint8_t *buf = g_isi_msg_data(msg);

	if (buf == NULL || g_isi_msg_data_len(msg) < offset)
		return FALSE;

	if (byte != NULL)
		*byte = buf[offset];

	return TRUE;
}

gboolean g_isi_msg_data_get_word(const GIsiMessage *msg, unsigned offset,
					uint16_t *word)
{
	const uint8_t *buf = g_isi_msg_data(msg);
	uint16_t val;

	if (buf == NULL || g_isi_msg_data_len(msg) < offset + 1)
		return FALSE;

	memcpy(&val, buf + offset, sizeof(uint16_t));

	if (word != NULL)
		*word = ntohs(val);

	return TRUE;
}

gboolean g_isi_msg_data_get_struct(const GIsiMessage *msg, unsigned offset,
					const void **type, size_t len)
{
	if (g_isi_msg_data_len(msg) < offset + len)
		return FALSE;

	if (type != NULL)
		*type = g_isi_msg_data(msg) + offset;

	return TRUE;
}
