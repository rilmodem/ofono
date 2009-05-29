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

struct ofono_modem_attribute_ops;

struct ofono_modem {
	int		id;
	char		*path;

	void 		*userdata;

	GSList *ss_control_list;

	struct ofono_modem_data *modem_info;
	struct network_registration_data *network_registration;
	struct voicecalls_data *voicecalls;
	struct call_forwarding_data *call_forwarding;
	struct ussd_data *ussd;
	struct call_settings_data *call_settings;
	struct call_waiting_data *call_waiting;
	struct call_meter_data *call_meter;
	struct call_barring_data *call_barring;
	struct cssn_data *cssn;
	struct sim_manager_data *sim_manager;
	struct sms_manager_data *sms_manager;
};

struct ofono_modem *modem_create(int id, struct ofono_modem_attribute_ops *ops);
void modem_remove(struct ofono_modem *modem);

void modem_add_interface(struct ofono_modem *modem, const char *interface);
void modem_remove_interface(struct ofono_modem *modem, const char *interface);

unsigned int modem_alloc_callid(struct ofono_modem *modem);
void modem_release_callid(struct ofono_modem *modem, int id);
