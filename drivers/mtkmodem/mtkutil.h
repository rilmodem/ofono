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

#ifndef MTKUTIL_H
#define MTKUTIL_H

#include <ofono/types.h>
#include <gril.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ofono_modem;

void mtk_set_attach_state(struct ofono_modem *modem, ofono_bool_t attached);
void mtk_detach_received(struct ofono_modem *modem);

void mtk_reset_all_modems(void);

struct mtk_gprs_data {
	GRil *gril;
	struct ofono_modem *modem;
};

const char *mtk_request_id_to_string(int req);
const char *mtk_unsol_request_to_string(int req);

#ifdef __cplusplus
}
#endif

#endif /* MTKUTIL_H */
