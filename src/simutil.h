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
	SIM_EFPL_FILEID = 0x2f05,
	SIM_EFLI_FILEID = 0x6f05,
	SIM_EFMSISDN_FILEID = 0x6f40,
	SIM_EFSPN_FILEID = 0x6f46,
	SIM_EFSDN_FILEID = 0x6f49,
	SIM_EFAD_FILEID = 0x6fad,
	SIM_EFPNN_FILEID = 0x6fc5,
	SIM_EFOPL_FILEID = 0x6fc6,
	SIM_EFMBDN_FILEID = 0x6fc7,
	SIM_EFMBI_FILEID = 0x6fc9,
	SIM_EFMWIS_FILEID = 0x6fca,
	SIM_EFSPDI_FILEID = 0x6fcd,
	SIM_EFECC_FILEID = 0x6fb7,
	SIM_EFCBMIR_FILEID = 0x6f50,
	SIM_EFCBMI_FILEID = 0x6f45,
	SIM_EFCBMID_FILEID = 0x6f48,
};

/* 51.011 Section 9.3 */
enum sim_file_access {
	SIM_FILE_ACCESS_ALWAYS = 0,
	SIM_FILE_ACCESS_CHV1 = 1,
	SIM_FILE_ACCESS_CHV2 = 2,
	SIM_FILE_ACCESS_RESERVED = 3,
	SIM_FILE_ACCESS_ADM = 4,
	SIM_FILE_ACCESS_NEVER = 15,
};

#define SIM_EFSPN_DC_HOME_PLMN_BIT 0x1
#define SIM_EFSPN_DC_ROAMING_SPN_BIT 0x2

struct sim_eons_operator_info {
	char *longname;
	gboolean long_ci;
	char *shortname;
	gboolean short_ci;
	char *info;
};

struct sim_ef_info {
	unsigned short id;
	unsigned short parent;
	unsigned char file_type;
	unsigned char size;
	enum sim_file_access perm_read;
	enum sim_file_access perm_update;
};

#define ROOTMF 0x3F00

struct sim_eons *sim_eons_new(int pnn_records);
void sim_eons_add_pnn_record(struct sim_eons *eons, int record,
				const guint8 *tlv, int length);
gboolean sim_eons_pnn_is_empty(struct sim_eons *eons);
void sim_eons_add_opl_record(struct sim_eons *eons,
				const guint8 *contents, int length);
void sim_eons_optimize(struct sim_eons *eons);
const struct sim_eons_operator_info *sim_eons_lookup_with_lac(
						struct sim_eons *eons,
						const char *mcc,
						const char *mnc,
						guint16 lac);
const struct sim_eons_operator_info *sim_eons_lookup(struct sim_eons *eons,
						const char *mcc,
						const char *mnc);
void sim_eons_free(struct sim_eons *eons);

struct sim_spdi *sim_spdi_new(const guint8 *tlv, int length);
gboolean sim_spdi_lookup(struct sim_spdi *spdi,
				const char *mcc, const char *mnc);
void sim_spdi_free(struct sim_spdi *spdi);

static inline enum sim_file_access file_access_condition_decode(int bcd)
{
	if (bcd >= 4 && bcd <= 14)
		return SIM_FILE_ACCESS_ADM;
	return bcd;
}

gboolean sim_adn_parse(const unsigned char *data, int length,
			struct ofono_phone_number *ph, char **identifier);
void sim_adn_build(unsigned char *data, int length,
			const struct ofono_phone_number *ph,
			const char *identifier);

struct sim_ef_info *sim_ef_db_lookup(unsigned short efid);

gboolean sim_parse_3g_get_response(const unsigned char *data, int len,
					int *file_len, int *record_len,
					int *structure, unsigned char *access,
					unsigned short *efid);

gboolean sim_parse_2g_get_response(const unsigned char *response, int len,
					int *file_len, int *record_len,
					int *structure, unsigned char *access);
