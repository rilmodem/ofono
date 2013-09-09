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

#ifndef __GRILUNSOL_H
#define __GRILUNSOL_H

#include <ofono/types.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct unsol_data_call_list {
	guint version;
	guint num;
	GSList *call_list;
};

struct data_call {
    guint status;
    guint retry;
    guint cid;
    guint active;
    char *type;
    char *ifname;
    char *addresses;
    char *dnses;
    char *gateways;
};

void g_ril_unsol_free_data_call_list(struct unsol_data_call_list *unsol);

struct unsol_data_call_list *g_ril_unsol_parse_data_call_list(GRil *gril,
					struct ril_msg *message,
					struct ofono_error *error);

#ifdef __cplusplus
}
#endif

#endif /* __GRILUNSOL_H */
