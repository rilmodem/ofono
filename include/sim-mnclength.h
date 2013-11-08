/*
 *
 *  oFono - Open Telephony stack for Linux
 *
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
 *  Copyright (C) 2013  Canonical Ltd.
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

#ifndef OFONO_SIM_MNCLENGTH_H
#define OFONO_SIM_MNCLENGTH_H

#ifdef __cplusplus
extern "C" {
#endif


struct ofono_sim_mnclength_driver {
	const char *name;
	int (*get_mnclength)(const char *imsi);
};

int ofono_sim_mnclength_driver_register(
			struct ofono_sim_mnclength_driver *driver);
void ofono_sim_mnclength_driver_unregister(
			const struct ofono_sim_mnclength_driver *driver);


#ifdef __cplusplus
}
#endif

#endif /* OFONO_SIM_MNCLENGTH_H */
