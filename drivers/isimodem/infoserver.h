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

#ifndef __OFONO_ISI_INFOSERVER_H
#define __OFONO_ISI_INFOSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct isi_infoserver;

struct isi_infoserver *isi_infoserver_create(struct ofono_modem *modem,
						void *data);

void isi_infoserver_destroy(struct isi_infoserver *self);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_ISI_INFOSERVER_H */
