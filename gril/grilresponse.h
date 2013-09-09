/*
 *
 *  RIL chat library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  Canonical Ltd.
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

#ifndef __GRIL_RESPONSE_H
#define __GRIL_RESPONSE_H

#ifdef __cplusplus
extern "C" {
#endif

struct _GRilResponse {
	GSList *lines;
	char *final_or_pdu;
};

typedef struct _GRilResponse GRilResponse;

#define G_RIL_RESPONSE_LINE_LENGTH_MAX 2048

const char *g_ril_final_response(GRilResponse *response);
const char *g_ril_response_pdu(GRilResponse *response);

#ifdef __cplusplus
}
#endif

#endif /* __GRIL_RESPONSE_H */
