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

enum sim_fileid {
	SIM_EFMSISDN_FILEID = 0x6f40,
	SIM_EFSPN_FILEID = 0x6f46,
	SIM_EFPNN_FILEID = 0x6fc5,
	SIM_EFOPL_FILEID = 0x6fc6,
	SIM_EFSPDI_FILEID = 0x6fcd,
};

#define SIM_EFSPN_DC_HOME_PLMN_BIT 0x1
#define SIM_EFSPN_DC_ROAMING_SPN_BIT 0x2

struct sim_spdi {
	GSList *operators;
};

struct sim_eons_operator_info {
	char *longname;
	gboolean long_ci;
	char *shortname;
	gboolean short_ci;
	char *info;
};

struct sim_eons {
	struct sim_eons_operator_info *pnn_list;
	GSList *opl_list;
	gboolean pnn_valid;
	int pnn_max;
};

struct sim_eons *sim_eons_new(int pnn_records);
void sim_eons_add_pnn_record(struct sim_eons *eons, int record,
				const guint8 *tlv, int length);
gboolean sim_eons_pnn_is_empty(struct sim_eons *eons);
void sim_eons_add_opl_record(struct sim_eons *eons,
				const guint8 *tlv, int length);
void sim_eons_optimize(struct sim_eons *eons);
struct sim_eons_operator_info *sim_eons_lookup(struct sim_eons *eons,
						const char *mcc,
						const char *mnc, guint16 lac);
void sim_eons_free(struct sim_eons *eons);

struct sim_spdi *sim_spdi_new(const guint8 *tlv, int length);
gboolean sim_spdi_lookup(struct sim_spdi *spdi,
				const char *mcc, const char *mnc);
void sim_spdi_free(struct sim_spdi *spdi);
