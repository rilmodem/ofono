/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013  Canonical Ltd.
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

#ifndef __GRILREQUEST_H
#define __GRILREQUEST_H

#include <ofono/types.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct req_deactivate_data_call {
	guint cid;
	guint reason;
};

struct req_setup_data_call {
	guint tech;
	guint data_profile;
	gchar *apn;
	gchar *username;
	gchar *password;
	guint auth_type;
	guint protocol;
};

gboolean g_ril_request_deactivate_data_call(GRil *gril,
				const struct req_deactivate_data_call *req,
				struct parcel *rilp,
				struct ofono_error *error);

gboolean g_ril_request_setup_data_call(GRil *gril,
					const struct req_setup_data_call *req,
					struct parcel *rilp,
					struct ofono_error *error);

#ifdef __cplusplus
}
#endif

#endif /* __GRILREQUEST_H */
