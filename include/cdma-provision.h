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

#ifndef __OFONO_CDMA_PROVISION_H
#define __OFONO_CDMA_PROVISION_H

#ifdef __cplusplus
extern "C" {
#endif

struct ofono_cdma_provision_driver {
	const char *name;
	int priority;
	int (*get_provider_name)(const char *sid, char **name);
};

int ofono_cdma_provision_driver_register(
		const struct ofono_cdma_provision_driver *driver);
void ofono_cdma_provision_driver_unregister(
		const struct ofono_cdma_provision_driver *driver);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_CDMA_PROVISION_H */
