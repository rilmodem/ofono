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

struct ril_gprs_data {
	GRil *ril;
	struct ofono_modem *modem;
	gboolean ofono_attached;
	unsigned int max_cids;
	int rild_status;
	int tech;
	int state_changed_unsol;
	int pending_deact_req;
	guint status_retry_cb_id;
};

int ril_gprs_probe(struct ofono_gprs *gprs, unsigned int vendor, void *data);
void ril_gprs_remove(struct ofono_gprs *gprs);
void ril_gprs_start(GRil *ril, struct ofono_gprs *gprs,
			struct ril_gprs_data *gd);
gboolean ril_gprs_set_attached_cb(gpointer user_data);
void ril_gprs_registration_status(struct ofono_gprs *gprs,
					ofono_gprs_status_cb_t cb, void *data);
void ril_gprs_set_ia_apn(struct ofono_gprs *gprs, const char *apn,
				enum ofono_gprs_proto proto, const char *user,
				const char *passwd, const char *mccmnc,
				ofono_gprs_cb_t cb, void *data);
