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

#ifndef __GISI_MODEM_H
#define __GISI_MODEM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*GIsiDebugFunc) (const void *restrict data, size_t len,
		void *opaque);

typedef struct _GIsiModem GIsiModem;

static inline unsigned g_isi_modem_index(GIsiModem *m)
{
	return (uintptr_t)m;
}

GIsiModem *g_isi_modem_by_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_MODEM_H */
