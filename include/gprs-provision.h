/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
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

#ifndef __OFONO_GPRS_PROVISION_H
#define __OFONO_GPRS_PROVISION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "gprs-context.h"

struct ofono_gprs_provision_data {
	enum ofono_gprs_context_type type;
	enum ofono_gprs_proto proto;
	char *name;
	char *apn;
	char *username;
	char *password;
	enum ofono_gprs_auth_method auth_method;
	char *message_proxy;
	char *message_center;
};

struct ofono_gprs_provision_driver {
	const char *name;
	int priority;
	int (*get_settings)(const char *mcc, const char *mnc, const char *spn,
				struct ofono_gprs_provision_data **settings,
				int *count);
};

int ofono_gprs_provision_driver_register(
			const struct ofono_gprs_provision_driver *driver);
void ofono_gprs_provision_driver_unregister(
			const struct ofono_gprs_provision_driver *driver);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_GPRS_PROVISION_H */
