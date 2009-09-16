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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <glib.h>
#include <arpa/inet.h>

#include "iter.h"

static inline void bcd_to_mccmnc(const uint8_t *bcd, char *mcc, char *mnc)
{
	mcc[0] = '0' + (bcd[0] & 0x0F);
	mcc[1] = '0' + ((bcd[0] & 0xF0) >> 4);
	mcc[2] = '0' + (bcd[1] & 0x0F);
	mcc[3] = '\0';

	mnc[0] = '0' + (bcd[2] & 0x0F);
	mnc[1] = '0' + ((bcd[2] & 0xF0) >> 4);
	mnc[2] = (bcd[1] & 0xF0) == 0xF0 ? '\0' : '0' +
			(bcd[1] & 0xF0);
	mnc[3] = '\0';
}

bool g_isi_sb_iter_init(const void *restrict data, size_t len,
			GIsiSubBlockIter *iter, bool longhdr)
{
	if (!iter || !data || len == 0)
		return false;

	iter->start = (uint8_t *)data;
	iter->end = iter->start + len;
	iter->longhdr = longhdr;

	return true;
}

bool g_isi_sb_iter_is_valid(GIsiSubBlockIter *iter)
{
	if (!iter || iter->end - iter->start < (iter->longhdr ? 4 : 2))
		return false;

	if (iter->start + g_isi_sb_iter_get_len(iter) > iter->end)
		return false;

	return true;
}

int g_isi_sb_iter_get_id(GIsiSubBlockIter *iter)
{
	if (iter->longhdr) {
		uint16_t *hdr = (uint16_t *)iter->start;
		return (int)ntohs(hdr[0]);
	}

	return iter->start[0];
}

size_t g_isi_sb_iter_get_len(GIsiSubBlockIter *iter)
{
	if (iter->longhdr) {
		uint16_t *hdr = (uint16_t *)iter->start;
		return (size_t)ntohs(hdr[1]);
	}

	return iter->start[1];
}

bool g_isi_sb_iter_get_byte(GIsiSubBlockIter *iter, uint8_t *byte, int pos)
{
	if (pos > (int)g_isi_sb_iter_get_len(iter) || iter->start + pos > iter->end)
		return false;

	*byte = iter->start[pos];
	return true;
}

bool g_isi_sb_iter_get_word(GIsiSubBlockIter *iter, uint16_t *word, int pos)
{
	uint16_t val;

	if (pos + 1 > (int)g_isi_sb_iter_get_len(iter))
		return false;

	memcpy(&val, iter->start + pos, sizeof(uint16_t));
	*word = ntohs(val);
	return true;
}

bool g_isi_sb_iter_get_dword(GIsiSubBlockIter *iter, uint32_t *dword,
					int pos)
{
	uint32_t val;

	if (pos + 3 > (int)g_isi_sb_iter_get_len(iter))
		return false;

	memcpy(&val, iter->start + pos, sizeof(uint32_t));
	*dword = ntohl(val);
	return true;
}

bool g_isi_sb_iter_get_oper_code(GIsiSubBlockIter *iter, char *mcc,
						char *mnc, int pos)
{
	if (pos + 2 > (int)g_isi_sb_iter_get_len(iter))
		return false;

	bcd_to_mccmnc(iter->start + pos, mcc, mnc);
	return true;
}

bool g_isi_sb_iter_get_alpha_tag(GIsiSubBlockIter *iter, char **utf8,
					size_t len, int pos)
{
	uint8_t *ucs2 = NULL;

	if (pos > (int)g_isi_sb_iter_get_len(iter))
		return false;

	if (!utf8 || len == 0 || pos + len > g_isi_sb_iter_get_len(iter))
		return false;

	ucs2 = iter->start + pos;
	if (ucs2 + len > iter->end)
		return false;

	*utf8 = g_convert((const char *)ucs2, len, "UTF-8//TRANSLIT", "UCS-2BE",
				NULL, NULL, NULL);
	return utf8 != NULL;
}

bool g_isi_sb_iter_get_latin_tag(GIsiSubBlockIter *iter, char **latin,
				size_t len, int pos)
{
	uint8_t *str = NULL;

	if (pos > (int)g_isi_sb_iter_get_len(iter))
		return false;

	if (!latin || len == 0 || pos + len > g_isi_sb_iter_get_len(iter))
		return false;

	str = iter->start + pos;
	if (str + len > iter->end)
		return false;

	*latin = g_strndup((char *)str, len);

	return latin != NULL;
}

bool g_isi_sb_iter_next(GIsiSubBlockIter *iter)
{
	uint8_t len = g_isi_sb_iter_get_len(iter);

	if (len == 0)
		len = iter->longhdr ? 4 : 2;

	if (iter->start + len > iter->end)
		return false;

	iter->start += len;
	return true;
}
