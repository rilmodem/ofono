/*
 * Copyright (C) 2011 Joel Armstrong <jcarmst@sandia.gov>
 * Copyright (C) 2012 Canonical Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (`GPL') as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * Based on parcel implementation from https://bitbucket.org/floren/inferno
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>

/* Parcel-handling code */
#include <sys/types.h>
#include <string.h>
#include <endian.h>
#include <stdint.h>
#include <limits.h>

#include "parcel.h"

#define PAD_SIZE(s) (((s)+3)&~3)

typedef uint16_t char16_t;

void parcel_init(struct parcel *p)
{
	p->data = g_malloc0(sizeof(int32_t));
	p->size = 0;
	p->capacity = sizeof(int32_t);
	p->offset = 0;
	p->malformed = 0;
}

void parcel_grow(struct parcel *p, size_t size)
{
	char *new = g_realloc(p->data, p->capacity + size);
	p->data = new;
	p->capacity += size;
}

void parcel_free(struct parcel *p)
{
	g_free(p->data);
	p->size = 0;
	p->capacity = 0;
	p->offset = 0;
}

int32_t parcel_r_int32(struct parcel *p)
{
	int32_t ret;

	if (p->malformed)
		return 0;

	if (p->offset + sizeof(int32_t) > p->size) {
		ofono_error("%s: parcel is too small", __func__);
		p->malformed = 1;
		return 0;
	}

	ret = *((int32_t *) (void *) (p->data + p->offset));
	p->offset += sizeof(int32_t);
	return ret;
}

int parcel_w_int32(struct parcel *p, int32_t val)
{
	for (;;) {

		if (p->offset + sizeof(int32_t) < p->capacity) {
			/* There's enough space */
			*((int32_t *) (void *) (p->data + p->offset)) = val;
			p->offset += sizeof(int32_t);
			p->size += sizeof(int32_t);
			break;
		} else {
			/* Grow data and retry */
			parcel_grow(p, sizeof(int32_t));
		}
	}
	return 0;
}

int parcel_w_string(struct parcel *p, const char *str)
{
	gunichar2 *gs16;
	glong gs16_len;
	size_t len;
	size_t gs16_size;

	if (str == NULL) {
		parcel_w_int32(p, -1);
		return 0;
	}

	gs16 = g_utf8_to_utf16(str, -1, NULL, &gs16_len, NULL);

	if (parcel_w_int32(p, gs16_len) == -1)
		return -1;

	gs16_size = gs16_len * sizeof(char16_t);
	len = gs16_size + sizeof(char16_t);
	for (;;) {
		size_t padded = PAD_SIZE(len);

		if (p->offset + len < p->capacity) {
			/* There's enough space */
			memcpy(p->data + p->offset, gs16, gs16_size);
			*((char16_t *) (void *)
				(p->data + p->offset + gs16_size)) = 0;
			p->offset += padded;
			p->size += padded;
			if (padded != len) {

#if BYTE_ORDER == BIG_ENDIAN
				static const uint32_t mask[4] = {
					0x00000000, 0xffffff00,
					0xffff0000, 0xff000000
				};
#endif
#if BYTE_ORDER == LITTLE_ENDIAN
				static const uint32_t mask[4] = {
					0x00000000, 0x00ffffff,
					0x0000ffff, 0x000000ff
				};
#endif

				*((uint32_t *) (void *)
					(p->data + p->offset - 4)) &=
							mask[padded - len];
			}
			break;

		} else {
			/* Grow data and retry */
			parcel_grow(p, padded);
		}
	}

	g_free(gs16);
	return 0;
}

char *parcel_r_string(struct parcel *p)
{
	char *ret;
	int len16 = parcel_r_int32(p);
	int strbytes;

	if (p->malformed)
		return NULL;

	/* This is how a null string is sent */
	if (len16 < 0)
		return NULL;

	strbytes = PAD_SIZE((len16 + 1) * sizeof(char16_t));
	if (p->offset + strbytes > p->size) {
		ofono_error("%s: parcel is too small", __func__);
		p->malformed = 1;
		return NULL;
	}

	ret = g_utf16_to_utf8((gunichar2 *) (void *) (p->data + p->offset),
				len16, NULL, NULL, NULL);
	if (ret == NULL) {
		ofono_error("%s: wrong UTF16 coding", __func__);
		p->malformed = 1;
		return NULL;
	}

	p->offset += strbytes;

	return ret;
}

int parcel_w_raw(struct parcel *p, const void *data, size_t len)
{
	if (data == NULL) {
		parcel_w_int32(p, -1);
		return 0;
	}

	parcel_w_int32(p, len);

	for (;;) {

		if (p->offset + len < p->capacity) {
			/* There's enough space */
			memcpy(p->data + p->offset, data, len);
			p->offset += len;
			p->size += len;
			break;
		} else {
			/* Grow data and retry */
			parcel_grow(p, len);
		}
	}
	return 0;
}

void *parcel_r_raw(struct parcel *p, int *len)
{
	char *ret;

	*len = parcel_r_int32(p);

	if (p->malformed || *len <= 0)
		return NULL;

	if (p->offset + *len > p->size) {
		ofono_error("%s: parcel is too small", __func__);
		p->malformed = 1;
		return NULL;
	}

	ret = g_try_malloc0(*len);
	if (ret == NULL) {
		ofono_error("%s: out of memory (%d bytes)", __func__, *len);
		return NULL;
	}

	memcpy(ret, p->data + p->offset, *len);
	p->offset += *len;

	return ret;
}

size_t parcel_data_avail(struct parcel *p)
{
	return p->size - p->offset;
}

struct parcel_str_array *parcel_r_str_array(struct parcel *p)
{
	int i;
	struct parcel_str_array *str_arr;
	int num_str = parcel_r_int32(p);

	if (p->malformed || num_str <= 0)
		return NULL;

	str_arr = g_try_malloc0(sizeof(*str_arr) + num_str * sizeof(char *));
	if (str_arr == NULL)
		return NULL;

	str_arr->num_str = num_str;
	for (i = 0; i < num_str; ++i)
		str_arr->str[i] = parcel_r_string(p);

	if (p->malformed) {
		parcel_free_str_array(str_arr);
		return NULL;
	}

	return str_arr;
}

void parcel_free_str_array(struct parcel_str_array *str_arr)
{
	if (str_arr) {
		int i;
		for (i = 0; i < str_arr->num_str; ++i)
			g_free(str_arr->str[i]);
		g_free(str_arr);
	}
}
