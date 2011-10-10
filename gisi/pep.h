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

#ifndef __GISI_PEP_H
#define __GISI_PEP_H

#include "modem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _GIsiPEP GIsiPEP;
typedef void (*GIsiPEPCallback)(GIsiPEP *pep, void *opaque);

GIsiPEP *g_isi_pep_create(GIsiModem *modem, GIsiPEPCallback cb, void *data);
void g_isi_pep_destroy(GIsiPEP *pep);
uint16_t g_isi_pep_get_object(const GIsiPEP *pep);
unsigned g_isi_pep_get_ifindex(const GIsiPEP *pep);
char *g_isi_pep_get_ifname(const GIsiPEP *pep, char *ifname);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_PEP_H */
