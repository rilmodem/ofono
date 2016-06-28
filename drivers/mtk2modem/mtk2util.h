/*
 *
 *  MTK2 driver for ofono/rilmodem
 *
 *  Copyright (C) 2016  Canonical Ltd.
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

#ifndef MTK2UTIL_H
#define MTK2UTIL_H

#include <ofono/types.h>
#include <gril.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *mtk2_request_id_to_string(int req);
const char *mtk2_unsol_request_to_string(int req);

#ifdef __cplusplus
}
#endif

#endif /* MTK2UTIL_H */
