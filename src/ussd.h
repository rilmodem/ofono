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

typedef gboolean (*ss_control_cb_t)(struct ofono_modem *modem,
					enum ss_control_type type,
					const char *sc,
					const char *sia, const char *sib,
					const char *sic, const char *dn,
					DBusMessage *msg);

typedef gboolean (*ss_passwd_cb_t)(struct ofono_modem *modem, const char *sc,
					const char *old, const char *new,
					DBusMessage *msg);

gboolean ss_control_register(struct ofono_modem *modem, const char *str,
				ss_control_cb_t cb);

void ss_control_unregister(struct ofono_modem *modem, const char *str,
				ss_control_cb_t cb);

gboolean ss_passwd_register(struct ofono_modem *modem, const char *str,
				ss_passwd_cb_t cb);

void ss_passwd_unregister(struct ofono_modem *modem, const char *str,
				ss_passwd_cb_t cb);
