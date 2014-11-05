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

#ifndef MTKSETTINGS_H
#define MTKSETTINGS_H

#include <ofono/types.h>

#include "gril.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mtk_settings_data;

struct mtk_settings_data *mtk_settings_create(struct ofono_modem *modem,
						GRil *ril, ofono_bool_t has_3g);
void mtk_settings_remove(struct mtk_settings_data *msd);

#ifdef __cplusplus
}
#endif

#endif /* MTKSETTINGS_H */
