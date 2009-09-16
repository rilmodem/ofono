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

#ifndef __GISI_ITER_H
#define __GISI_ITER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

struct _GIsiSubBlockIter {
	uint8_t *start;
	uint8_t *end;
	bool longhdr;
};
typedef struct _GIsiSubBlockIter GIsiSubBlockIter;

bool g_isi_sb_iter_init(const void *restrict data, size_t len,
			GIsiSubBlockIter *iter, bool longhdr);
bool g_isi_sb_iter_is_valid(GIsiSubBlockIter *iter);
bool g_isi_sb_iter_next(GIsiSubBlockIter *iter);

int g_isi_sb_iter_get_id(GIsiSubBlockIter *iter);
size_t g_isi_sb_iter_get_len(GIsiSubBlockIter *iter);

bool g_isi_sb_iter_get_byte(GIsiSubBlockIter *iter, uint8_t *byte, int pos);
bool g_isi_sb_iter_get_word(GIsiSubBlockIter *iter, uint16_t *word, int pos);
bool g_isi_sb_iter_get_dword(GIsiSubBlockIter *iter, uint32_t *dword, int pos);
bool g_isi_sb_iter_get_oper_code(GIsiSubBlockIter *iter, char *mcc,
					char *mnc, int pos);
bool g_isi_sb_iter_get_alpha_tag(GIsiSubBlockIter *iter, char **utf8,
					size_t len, int pos);
bool g_isi_sb_iter_get_latin_tag(GIsiSubBlockIter *iter, char **ascii,
					size_t len, int pos);

#ifdef __cplusplus
}
#endif

#endif /* __GISI_ITER_H */
