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

struct switch_data;

struct radio_data {
	struct ofono_radio_settings_driver *virt_tbl;
	GRil *ril;
	struct ofono_modem *modem;
	struct ofono_radio_settings *radio_settings;
	gboolean fast_dormancy;
	gboolean pending_fd;
	int rat_mode;
	int pending_mode;
	unsigned available_rats;
	int ril_rats;
	char modem_uuid[RIL_MAX_UUID_LENGTH];
	struct switch_data *switch_d;
	unsigned gprs_atom_watch;
};

struct switch_data {
	struct radio_data *rd_1;
	struct radio_data *rd_2;
	enum ofono_radio_access_mode mode_to_switch;
	int pending_msgs;
	struct cb_data *cbd;
};

void ril_delayed_register(const struct ofono_error *error, void *user_data);
void ril_radio_settings_remove(struct ofono_radio_settings *rs);
void ril_query_rat_mode(struct ofono_radio_settings *rs,
			ofono_radio_settings_rat_mode_query_cb_t cb,
			void *data);
void ril_set_rat_mode(struct ofono_radio_settings *rs,
			enum ofono_radio_access_mode mode,
			ofono_radio_settings_rat_mode_set_cb_t cb,
			void *data);
void ril_query_fast_dormancy(struct ofono_radio_settings *rs,
			ofono_radio_settings_fast_dormancy_query_cb_t cb,
			void *data);
void ril_set_fast_dormancy(struct ofono_radio_settings *rs,
				ofono_bool_t enable,
				ofono_radio_settings_fast_dormancy_set_cb_t cb,
				void *data);
void ril_query_available_rats(struct ofono_radio_settings *rs,
			ofono_radio_settings_available_rats_query_cb_t cb,
			void *data);
