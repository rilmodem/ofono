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

struct sim_pnn_operator {
	char *longname;
	gboolean long_ci;
	char *shortname;
	gboolean short_ci;
	char *info;
};

void sim_pnn_operator_free(struct sim_pnn_operator *oper);

const guint8 *ber_tlv_find_by_tag(const guint8 *pdu, guint8 in_tag,
					int in_len, int *out_len);
char *sim_network_name_parse(const unsigned char *buffer, int length,
				gboolean *add_ci);
gboolean sim_pnn_operator_parse(struct sim_pnn_operator *oper,
				const guint8 *tlv, int length);
