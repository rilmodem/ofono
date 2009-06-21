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

typedef void (*update_spn_cb)(struct ofono_modem *modem, const char *spn,
		int home_plmn_dpy, int roaming_spn_dpy);

void ofono_sim_manager_init(struct ofono_modem *modem);
void ofono_sim_manager_exit(struct ofono_modem *modem);
int ofono_spn_update_notify_register(struct ofono_modem *modem,
		update_spn_cb cb);
int ofono_spn_update_notify_unregister(struct ofono_modem *modem,
		update_spn_cb cb);

int ofono_operator_in_spdi(struct ofono_modem *modem,
				const struct ofono_network_operator *op);
