/*
 *
 *  oFono - Open Source Telephony
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

struct powerd_data {
	GRil *ril;
};

int ril_powerd_probe(struct ofono_powerd *powerd,
			unsigned int vendor, void *user);
void ril_powerd_remove(struct ofono_powerd *powerd);
void ril_powerd_set_display_state(struct ofono_powerd *powerd, gboolean on,
					ofono_powerd_cb_t cb, void *user_data);
