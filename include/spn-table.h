/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2014  Canonical Ltd.
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

#ifndef OFONO_SPN_TABLE_H
#define OFONO_SPN_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif

struct ofono_spn_table_driver {
	const char *name;
	const char *(*get_spn)(const char *numeric);
};

const char *__ofono_spn_table_get_spn(const char *numeric);

int ofono_spn_table_driver_register(struct ofono_spn_table_driver *driver);

void ofono_spn_table_driver_unregister(
			const struct ofono_spn_table_driver *driver);

#ifdef __cplusplus
}
#endif

#endif /* OFONO_SPN_TABLE_H */
