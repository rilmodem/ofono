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

typedef void (*ofono_sim_ready_notify_cb_t)(struct ofono_modem *modem);

typedef void (*ofono_sim_read_binary_cb_t)(struct ofono_modem *modem,
						const struct ofono_error *error,
						const unsigned char *data,
						int len, void *userdata);

void ofono_sim_manager_init(struct ofono_modem *modem);
void ofono_sim_manager_exit(struct ofono_modem *modem);

gboolean ofono_operator_in_spdi(struct ofono_modem *modem,
				const struct ofono_network_operator *op);
const char *ofono_sim_get_imsi(struct ofono_modem *modem);

int ofono_sim_ready_notify_register(struct ofono_modem *modem,
					ofono_sim_ready_notify_cb_t cb);
void ofono_sim_ready_notify_unregister(struct ofono_modem *modem,
					ofono_sim_ready_notify_cb_t cb);

const char *ofono_operator_name_sim_override(struct ofono_modem *modem,
						const char *mcc,
						const char *mnc);
int ofono_sim_get_ready(struct ofono_modem *modem);
void ofono_sim_set_ready(struct ofono_modem *modem);
