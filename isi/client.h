/*
 * This file is part of oFono - Open Source Telephony
 *
 * Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).
 *
 * Contact: Rémi Denis-Courmont <remi.denis-courmont@nokia.com>
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

#ifndef OFONO_PHONET_CLIENT_H
#define OFONO_PHONET_CLIENT_H 1
#include <stdint.h>
#include <stdbool.h>

struct isi_client;
struct isi_client *isi_client_create(uint8_t resource);
void isi_client_destroy(struct isi_client *client);
int isi_client_error(const struct isi_client *client);

typedef bool (*isi_client_cb_t)(struct isi_client *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque);
struct isi_request;
struct isi_request *isi_request_make(struct isi_client *, const void *, size_t,
				unsigned timeout, isi_client_cb_t, void *);
void isi_request_cancel(struct isi_request *req);

typedef void (*isi_ind_cb_t) (struct isi_client *client,
				const void *restrict data, size_t len,
				uint16_t object, void *opaque);
int isi_subscribe(struct isi_client *client, uint8_t type,
				isi_ind_cb_t, void *);
void isi_unsubscribe(struct isi_client *client, uint8_t type);
#endif
