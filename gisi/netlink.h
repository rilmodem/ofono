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

#include <stdbool.h>
#include <stdint.h>
#include <gisi/modem.h>

#ifndef __GPHONET_NETLINK_H
#define __GPHONET_NETLINK_H

#ifdef __cplusplus
extern "C" {
#endif

struct _GPhonetNetlink;
typedef struct _GPhonetNetlink GPhonetNetlink;

typedef void (*GPhonetNetlinkFunc)(bool up, uint8_t addr, GIsiModem *idx,
					void *data);

GPhonetNetlink *g_pn_netlink_start(GPhonetNetlinkFunc func, void *data);
void g_pn_netlink_stop(GPhonetNetlink *self);

#ifdef __cplusplus
}
#endif

#endif /* __GPHONET_NETLINK_H */
