/*
 *
 *  MTK driver for ofono/rilmodem
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

#ifndef MTKUNSOL_H
#define MTKUNSOL_H

#include <ofono/types.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct unsol_call_indication {
	int call_id;
	int call_mode;
	int seq_number;
};

void g_mtk_unsol_free_call_indication(struct unsol_call_indication *unsol);

struct unsol_call_indication *g_mtk_unsol_parse_incoming_call_indication(
					GRil *gril, struct ril_msg *message);

/* Returns a session id that must be used in resume registration request */
int g_mtk_unsol_parse_registration_suspended(GRil *gril,
						const struct ril_msg *message);

#ifdef __cplusplus
}
#endif

#endif /* MTKUNSOL_H */
