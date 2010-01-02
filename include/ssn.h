/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_SSN_H
#define __OFONO_SSN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <ofono/types.h>

struct ofono_ssn;

struct ofono_ssn_driver {
	const char *name;
	int (*probe)(struct ofono_ssn *ssn, unsigned int vendor, void *data);
	void (*remove)(struct ofono_ssn *ssn);
};

/* SSN notifications (CSSI and CSSU).  */
void ofono_ssn_cssi_notify(struct ofono_ssn *ssn, int code, int index);
void ofono_ssn_cssu_notify(struct ofono_ssn *ssn, int code, int index,
				const struct ofono_phone_number *number);

int ofono_ssn_driver_register(const struct ofono_ssn_driver *d);
void ofono_ssn_driver_unregister(const struct ofono_ssn_driver *d);

struct ofono_ssn *ofono_ssn_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data);

void ofono_ssn_register(struct ofono_ssn *ssn);
void ofono_ssn_remove(struct ofono_ssn *ssn);

void ofono_ssn_set_data(struct ofono_ssn *ssn, void *data);
void *ofono_ssn_get_data(struct ofono_ssn *ssn);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_SSN_H */
