/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

struct idmap;

struct idmap *idmap_new(unsigned int size);
void idmap_free(struct idmap *idmap);
void idmap_put(struct idmap *idmap, unsigned int id);
void idmap_take(struct idmap *idmap, unsigned int id);
unsigned int idmap_alloc(struct idmap *idmap);
unsigned int idmap_alloc_next(struct idmap *idmap, unsigned int last);
struct idmap *idmap_new_from_range(unsigned int min, unsigned int max);
unsigned int idmap_get_min(struct idmap *idmap);
unsigned int idmap_get_max(struct idmap *idmap);
