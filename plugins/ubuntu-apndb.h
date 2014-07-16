/*
 *
 *  ofono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *                2013 Simon Busch <morphis@gravedo.de>
 *  Copyright (C) 2014 Canonical Ltd.
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

struct apndb_provision_data {
	struct ofono_gprs_provision_data gprs_data;
	gboolean mvno;
};

void ubuntu_apndb_ap_free(gpointer data);

GSList *ubuntu_apndb_lookup_apn(const char *mcc, const char *mnc,
			const char *spn, const char *imsi, const char *gid1,
			GError **error);
