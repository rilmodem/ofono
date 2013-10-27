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
	ret = *((int32_t *) (p->data + p->offset));
	p->offset += sizeof(int32_t);
	return ret;
}

int parcel_w_int32(struct parcel *p, int32_t val)
{
	for (;;) {

		DBG("parcel_w_int32(%d): offset = %d, cap = %d, size = %d\n",
		    val, (int) p->offset, (int) p->capacity, (int) p->size);

		if (p->offset + sizeof(int32_t) < p->capacity) {
			/* There's enough space */
			*((int32_t *) (p->data + p->offset)) = val;
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

int parcel_w_string(struct parcel *p, char *str)
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

	if (parcel_w_int32(p, gs16_len) == -1) {
		return -1;
	}

	gs16_size = gs16_len * sizeof(char16_t);
	len = gs16_size + sizeof(char16_t);
	for (;;) {
		size_t padded = PAD_SIZE(len);

		DBG("parcel_w_string(\"%s\"): len %d offset %d, cap %d, size %d",
		    str, (int) len, (int) p->offset, (int) p->capacity, (int) p->size);
		if (p->offset + len < p->capacity) {
			/* There's enough space */
			memcpy(p->data + p->offset, gs16, gs16_size);
			*((char16_t *) (p->data + p->offset + gs16_size)) = 0;
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

				DBG("Writing %d bytes, padded to %d\n",
				    (int) len, (int) padded);

				*((uint32_t*)(p->data + p->offset - 4)) &=
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

char* parcel_r_string(struct parcel *p)
{
	char *ret;
	int len16 = parcel_r_int32(p);

	/* This is how a null string is sent */
	if (len16 < 0)
		return NULL;

	ret = g_utf16_to_utf8((gunichar2 *) (p->data + p->offset),
				len16, NULL, NULL, NULL);
	if (ret == NULL)
		return NULL;

	p->offset += PAD_SIZE((len16 + 1) * sizeof(char16_t));

	return ret;
}

size_t parcel_data_avail(struct parcel *p)
{
	return (p->size - p->offset);
}
