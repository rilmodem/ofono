/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

typedef void (*mo_ss_notify_cb)(int index, void *userdata);
typedef void (*mt_ss_notify_cb)(int index, const struct ofono_phone_number *ph,
				void *userdata);

void ofono_cssn_init(struct ofono_modem *modem);
void ofono_cssn_exit(struct ofono_modem *modem);
void ofono_mo_ss_register(struct ofono_modem *modem, enum ss_cssi code1,
		mo_ss_notify_cb cb, void *userdata);
void ofono_mo_ss_unregister(struct ofono_modem *modem, enum ss_cssi code1,
		mo_ss_notify_cb cb, void *userdata);
void ofono_mt_ss_register(struct ofono_modem *modem, enum ss_cssu code2,
		mt_ss_notify_cb cb, void *userdata);
void ofono_mt_ss_unregister(struct ofono_modem *modem, enum ss_cssu code2,
		mt_ss_notify_cb cb, void *userdata);
