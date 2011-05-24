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

#ifndef __OFONO_PRIVATE_NETWORK_H
#define __OFONO_PRIVATE_NETWORK_H

#ifdef __cplusplus
extern "C" {
#endif

struct ofono_private_network_settings {
	int fd;
	char *server_ip;
	char *peer_ip;
	char *primary_dns;
	char *secondary_dns;
};

typedef void (*ofono_private_network_cb_t)(
			const struct ofono_private_network_settings *settings,
			void *data);

struct ofono_private_network_driver {
	char *name;
	int (*request)(ofono_private_network_cb_t cb, void *data);
	void (*release)(int uid);
};

int ofono_private_network_driver_register(
			const struct ofono_private_network_driver *d);
void ofono_private_network_driver_unregister(
			const struct ofono_private_network_driver *d);

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_PRIVATE_NETWORK_H */
