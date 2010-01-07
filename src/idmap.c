/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
 *  Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
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

#define _GNU_SOURCE
#include <string.h>

#include <glib.h>

#include "idmap.h"

#define BITS_PER_LONG (sizeof(unsigned long) * 8)

struct idmap {
	unsigned long *bits;
	unsigned int size;
};

static inline int ffz(unsigned long word)
{
	return __builtin_ctzl(~word);
}

/* 
 * Stolen from linux kernel lib/find_next_bit.c
 */
static unsigned int find_next_zero_bit(const unsigned long *addr,
					unsigned int size,
				 	unsigned int offset)
{
	const unsigned long *p = addr + offset / BITS_PER_LONG;
	unsigned int result = offset & ~(BITS_PER_LONG-1);
	unsigned long tmp;

	if (offset >= size)
		return size;

	size -= result;
	offset %= BITS_PER_LONG;

	if (offset) {
		tmp = *(p++);
		tmp |= ~0UL >> (BITS_PER_LONG - offset);

		if (size < BITS_PER_LONG)
			goto found_first;

		if (~tmp)
			goto found_middle;

		size -= BITS_PER_LONG;
		result += BITS_PER_LONG;
	}

	while (size & ~(BITS_PER_LONG-1)) {
		if (~(tmp = *(p++)))
			goto found_middle;

		size -= BITS_PER_LONG;
		result += BITS_PER_LONG;
	}

	if (!size)
		return result;

	tmp = *p;

found_first:
	tmp |= ~0UL << size;

	if (tmp == ~0UL)	/* Are any bits zero? */
		return result + size;	/* Nope. */

found_middle:
	return result + ffz(tmp);
}

struct idmap *idmap_new(unsigned int size)
{
	struct idmap *ret = g_new0(struct idmap, 1);

	ret->bits = g_new0(unsigned long,
				(size + BITS_PER_LONG - 1) / BITS_PER_LONG);
	ret->size = size;

	return ret;
}

void idmap_free(struct idmap *idmap)
{
	g_free(idmap->bits);
	g_free(idmap);
}

void idmap_put(struct idmap *idmap, unsigned int bit)
{
	unsigned int offset = (bit - 1) / BITS_PER_LONG;

	bit -= 1;

	if (bit > idmap->size)
		return;

	bit %= BITS_PER_LONG;

	idmap->bits[offset] &= ~(1 << bit);
}

unsigned int idmap_alloc(struct idmap *idmap)
{
	unsigned int bit;
	unsigned int offset;

	bit = find_next_zero_bit(idmap->bits, idmap->size, 0);

	if (bit >= idmap->size)
		return 0;

	offset = bit / BITS_PER_LONG;
	idmap->bits[offset] |= 1 << (bit % BITS_PER_LONG);

	return bit + 1;
}

/*
 * Allocate the next bit skipping the first last bits
 */
unsigned int idmap_alloc_next(struct idmap *idmap, unsigned int last)
{
	unsigned int bit;
	unsigned int offset;

	bit = find_next_zero_bit(idmap->bits, idmap->size, last);

	if (bit >= idmap->size)
		return idmap_alloc(idmap);

	offset = bit / BITS_PER_LONG;
	idmap->bits[offset] |= 1 << (bit % BITS_PER_LONG);

	return bit + 1;
}
