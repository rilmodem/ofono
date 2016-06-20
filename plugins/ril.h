/*
 *
 *  oFono - Open Source Telephony - RIL-based devices
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

typedef const char *(*ril_get_driver_type_func)(enum ofono_atom_type atom);

int ril_create(struct ofono_modem *modem, enum ofono_ril_vendor vendor,
		GRilMsgIdToStrFunc request_id_to_string,
		GRilMsgIdToStrFunc unsol_request_to_string,
		ril_get_driver_type_func get_driver_type);
void ril_remove(struct ofono_modem *modem);
int ril_enable(struct ofono_modem *modem);
int ril_disable(struct ofono_modem *modem);
void ril_pre_sim(struct ofono_modem *modem);
void ril_post_sim(struct ofono_modem *modem);
void ril_post_online(struct ofono_modem *modem);
void ril_set_online(struct ofono_modem *modem, ofono_bool_t online,
			ofono_modem_online_cb_t callback, void *data);

GRil *ril_get_gril_c(struct ofono_modem *modem);
