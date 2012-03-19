/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __GISI_MESSAGE_H
#define __GISI_MESSAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <glib.h>

#include "phonet.h"

struct _GIsiVersion {
	int major;
	int minor;
};
typedef struct _GIsiVersion GIsiVersion;

struct _GIsiMessage {
	struct sockaddr_pn *addr;
	GIsiVersion *version;
	int error;
	const void *__restrict data;
	size_t len;
	void *private;
};
typedef struct _GIsiMessage GIsiMessage;

int g_isi_msg_version_major(const GIsiMessage *msg);
int g_isi_msg_version_minor(const GIsiMessage *msg);
int g_isi_msg_error(const GIsiMessage *msg);
const char *g_isi_msg_strerror(const GIsiMessage *msg);
uint8_t g_isi_msg_resource(const GIsiMessage *msg);
uint16_t g_isi_msg_object(const GIsiMessage *msg);

uint8_t g_isi_msg_id(const GIsiMessage *msg);
uint8_t g_isi_msg_utid(const GIsiMessage *msg);
size_t g_isi_msg_data_len(const GIsiMessage *msg);
const void *g_isi_msg_data(const GIsiMessage *msg);

gboolean g_isi_msg_data_get_byte(const GIsiMessage *msg, unsigned offset,
					uint8_t *byte);
gboolean g_isi_msg_data_get_word(const GIsiMessage *msg, unsigned offset,
					uint16_t *word);
gboolean g_isi_msg_data_get_struct(const GIsiMessage *msg, unsigned offset,
					const void **type, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_MESSAGE_H */
