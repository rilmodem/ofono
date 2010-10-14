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
#include <glib.h>
#include <arpa/inet.h>

#include "iter.h"

static inline void bcd_to_mccmnc(const uint8_t *restrict bcd,
					char *mcc, char *mnc)
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

void g_isi_sb_iter_init_full(GIsiSubBlockIter *iter, const void *restrict data,
				size_t len, size_t used, gboolean longhdr,
				uint16_t sub_blocks)
{
	if (!data)
		len = used = 0;

	iter->start = (uint8_t *)data + used;
	iter->end = iter->start + len;
	iter->longhdr = longhdr;
	iter->sub_blocks = len > used ? sub_blocks : 0;
}

void g_isi_sb_iter_init(GIsiSubBlockIter *iter, const void *restrict data,
			size_t len, size_t used)
{
	if (!data)
		len = used = 0;

	iter->start = (uint8_t *)data + used;
	iter->end = iter->start + len;
	iter->longhdr = FALSE;
	iter->sub_blocks = len > used ? iter->start[-1] : 0;
}

gboolean g_isi_sb_iter_is_valid(const GIsiSubBlockIter *iter)
{
	if (!iter)
		return FALSE;

	if (iter->sub_blocks == 0)
		return FALSE;

	if (iter->start + (iter->longhdr ? 4 : 2) > iter->end)
		return FALSE;

	if (iter->start + g_isi_sb_iter_get_len(iter) > iter->end)
		return FALSE;

	return TRUE;
}

int g_isi_sb_iter_get_id(const GIsiSubBlockIter *iter)
{
	if (iter->longhdr)
		return (iter->start[0] << 8) | (iter->start[1]);
	return iter->start[0];
}

size_t g_isi_sb_iter_get_len(const GIsiSubBlockIter *iter)
{
	if (iter->longhdr)
		return (iter->start[2] << 8) | (iter->start[3]);
	return iter->start[1];
}

gboolean g_isi_sb_iter_get_data(const GIsiSubBlockIter *restrict iter,
				void **data, unsigned pos)
{
	if ((size_t)pos > g_isi_sb_iter_get_len(iter)
		|| iter->start + pos > iter->end)
		return FALSE;
	*data = (void *)iter->start + pos;
	return TRUE;
}

gboolean g_isi_sb_iter_get_byte(const GIsiSubBlockIter *restrict iter,
				uint8_t *byte, unsigned pos)
{
	if ((size_t)pos > g_isi_sb_iter_get_len(iter)
		|| iter->start + pos > iter->end)
		return FALSE;
	*byte = iter->start[pos];
	return TRUE;
}

gboolean g_isi_sb_iter_get_word(const GIsiSubBlockIter *restrict iter,
				uint16_t *word, unsigned pos)
{
	uint16_t val;

	if (pos + 1 > g_isi_sb_iter_get_len(iter))
		return FALSE;

	memcpy(&val, iter->start + pos, sizeof(uint16_t));
	*word = ntohs(val);
	return TRUE;
}

gboolean g_isi_sb_iter_get_dword(const GIsiSubBlockIter *restrict iter,
					uint32_t *dword, unsigned pos)
{
	uint32_t val;

	if (pos + 3 > g_isi_sb_iter_get_len(iter))
		return FALSE;

	memcpy(&val, iter->start + pos, sizeof(uint32_t));
	*dword = ntohl(val);
	return TRUE;
}

gboolean g_isi_sb_iter_get_oper_code(const GIsiSubBlockIter *restrict iter,
					char *mcc, char *mnc, unsigned pos)
{
	if (pos + 2 > g_isi_sb_iter_get_len(iter))
		return FALSE;

	bcd_to_mccmnc(iter->start + pos, mcc, mnc);
	return TRUE;
}

gboolean g_isi_sb_iter_get_alpha_tag(const GIsiSubBlockIter *restrict iter,
					char **utf8, size_t len, unsigned pos)
{
	uint8_t *ucs2 = NULL;

	if (pos > g_isi_sb_iter_get_len(iter))
		return FALSE;

	if (!utf8 || len == 0 || pos + len > g_isi_sb_iter_get_len(iter))
		return FALSE;

	ucs2 = iter->start + pos;
	if (ucs2 + len > iter->end)
		return FALSE;

	*utf8 = g_convert((const char *)ucs2, len, "UTF-8//TRANSLIT", "UCS-2BE",
				NULL, NULL, NULL);
	return *utf8 != NULL;
}

gboolean g_isi_sb_iter_get_latin_tag(const GIsiSubBlockIter *restrict iter,
					char **latin, size_t len, unsigned pos)
{
	uint8_t *str = NULL;

	if (pos > g_isi_sb_iter_get_len(iter))
		return FALSE;

	if (!latin || len == 0 || pos + len > g_isi_sb_iter_get_len(iter))
		return FALSE;

	str = iter->start + pos;
	if (str + len > iter->end)
		return FALSE;

	*latin = g_strndup((char *)str, len);

	return *latin != NULL;
}

gboolean g_isi_sb_iter_next(GIsiSubBlockIter *iter)
{
	uint8_t len = g_isi_sb_iter_get_len(iter);

	if (len == 0)
		len = iter->longhdr ? 4 : 2;

	if (iter->sub_blocks == 0)
		return FALSE;

	if (iter->start + len > iter->end)
		return FALSE;

	iter->start += len;
	iter->sub_blocks--;

	return TRUE;
}
