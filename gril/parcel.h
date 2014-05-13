/*
 * Copyright © 2011 Joel Armstrong <jcarmst@sandia.gov>
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

#ifndef __PARCEL_H
#define __PARCEL_H

#include <stdlib.h>

struct parcel {
	char *data;
	size_t offset;
	size_t capacity;
	size_t size;
	int malformed;
};

void parcel_init(struct parcel *p);
void parcel_grow(struct parcel *p, size_t size);
void parcel_free(struct parcel *p);
int32_t parcel_r_int32(struct parcel *p);
int parcel_w_int32(struct parcel *p, int32_t val);
int parcel_w_string(struct parcel *p, const char *str);
char *parcel_r_string(struct parcel *p);
int parcel_w_raw(struct parcel *p, const void *data, size_t len);
void *parcel_r_raw(struct parcel *p,  int *len);
size_t parcel_data_avail(struct parcel *p);

#endif
