/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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
	SIM_EF_ICCID_FILEID = 0x2fe2,
	SIM_EFLI_FILEID = 0x6f05,
	SIM_EF_CPHS_MWIS_FILEID = 0x6f11,
	SIM_EF_CPHS_INFORMATION_FILEID = 0x6f16,
	SIM_EF_CPHS_MBDN_FILEID = 0x6f17,
	SIM_EFMSISDN_FILEID = 0x6f40,
	SIM_EFSPN_FILEID = 0x6f46,
	SIM_EFSDN_FILEID = 0x6f49,
	SIM_EFAD_FILEID = 0x6fad,
	SIM_EFPHASE_FILEID = 0x6fae,
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

enum ber_tlv_data_type {
	BER_TLV_DATA_TYPE_UNIVERSAL = 0,
	BER_TLV_DATA_TYPE_APPLICATION = 1,
	BER_TLV_DATA_TYPE_CONTEXT_SPECIFIC = 2,
	BER_TLV_DATA_TYPE_PRIVATE = 3,
};

enum ber_tlv_data_encoding_type {
	BER_TLV_DATA_ENCODING_TYPE_PRIMITIVE = 0,
	BER_TLV_DATA_ENCODING_TYPE_CONSTRUCTED = 1,
};

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

struct simple_tlv_iter {
	unsigned int max;
	unsigned int pos;
	const unsigned char *pdu;
	unsigned char tag;
	unsigned short len;
	const unsigned char *data;
};

struct comprehension_tlv_iter {
	unsigned int max;
	unsigned int pos;
	const unsigned char *pdu;
	unsigned short tag;
	gboolean cr;
	unsigned int len;
	const unsigned char *data;
};

struct ber_tlv_iter {
	unsigned int max;
	unsigned int pos;
	const unsigned char *pdu;
	unsigned int tag;
	enum ber_tlv_data_type class;
	enum ber_tlv_data_encoding_type encoding;
	unsigned int len;
	const unsigned char *data;
};

struct ber_tlv_builder {
	unsigned int max;
	unsigned int pos;
	unsigned char *pdu;
	struct ber_tlv_builder *parent;

	unsigned int tag;
	enum ber_tlv_data_type class;
	enum ber_tlv_data_encoding_type encoding;
	unsigned int len;
};

struct comprehension_tlv_builder {
	unsigned int max;
	unsigned int pos;
	unsigned char *pdu;
	unsigned int len;
	struct ber_tlv_builder *parent;
};

void simple_tlv_iter_init(struct simple_tlv_iter *iter,
				const unsigned char *pdu, unsigned int len);
gboolean simple_tlv_iter_next(struct simple_tlv_iter *iter);
unsigned char simple_tlv_iter_get_tag(struct simple_tlv_iter *iter);
unsigned short simple_tlv_iter_get_length(struct simple_tlv_iter *iter);
const unsigned char *simple_tlv_iter_get_data(struct simple_tlv_iter *iter);

void comprehension_tlv_iter_init(struct comprehension_tlv_iter *iter,
					const unsigned char *pdu,
					unsigned int len);
gboolean comprehension_tlv_iter_next(struct comprehension_tlv_iter *iter);
unsigned short comprehension_tlv_iter_get_tag(struct comprehension_tlv_iter *i);
gboolean comprehension_tlv_get_cr(struct comprehension_tlv_iter *iter);
unsigned int comprehension_tlv_iter_get_length(
					struct comprehension_tlv_iter *iter);
const unsigned char *comprehension_tlv_iter_get_data(
					struct comprehension_tlv_iter *iter);

void comprehension_tlv_iter_copy(struct comprehension_tlv_iter *from,
					struct comprehension_tlv_iter *to);

gboolean comprehension_tlv_builder_init(
				struct comprehension_tlv_builder *builder,
				unsigned char *pdu, unsigned int size);
gboolean comprehension_tlv_builder_next(
				struct comprehension_tlv_builder *builder,
				gboolean cr, unsigned short tag);
gboolean comprehension_tlv_builder_set_length(
				struct comprehension_tlv_builder *builder,
				unsigned int len);
unsigned char *comprehension_tlv_builder_get_data(
				struct comprehension_tlv_builder *builder);

void ber_tlv_iter_init(struct ber_tlv_iter *iter, const unsigned char *pdu,
			unsigned int len);
/*
 * Returns the tag value of the TLV.  Note that the tag value can be either
 * short (0-30) or long
 */
unsigned int ber_tlv_iter_get_tag(struct ber_tlv_iter *iter);

enum ber_tlv_data_type ber_tlv_iter_get_class(struct ber_tlv_iter *iter);
enum ber_tlv_data_encoding_type
	ber_tlv_iter_get_encoding(struct ber_tlv_iter *iter);

/*
 * This will return the short tag along with class and encoding information.
 * This is more convenient to use for TLV contents of SIM Elementary Files
 * and SIM toolkit since these elements only use short tags.  In case of an
 * error (e.g. not a short tag) a zero is returned.  According to ISO 7816,
 * a tag value of '00' is invalid.
 */
unsigned char ber_tlv_iter_get_short_tag(struct ber_tlv_iter *iter);
unsigned int ber_tlv_iter_get_length(struct ber_tlv_iter *iter);

const unsigned char *ber_tlv_iter_get_data(struct ber_tlv_iter *iter);

gboolean ber_tlv_iter_next(struct ber_tlv_iter *iter);
void ber_tlv_iter_recurse(struct ber_tlv_iter *iter,
				struct ber_tlv_iter *recurse);
void ber_tlv_iter_recurse_simple(struct ber_tlv_iter *iter,
					struct simple_tlv_iter *container);
void ber_tlv_iter_recurse_comprehension(struct ber_tlv_iter *iter,
					struct comprehension_tlv_iter *recurse);

gboolean ber_tlv_builder_init(struct ber_tlv_builder *builder,
				unsigned char *pdu, unsigned int size);
gboolean ber_tlv_builder_next(struct ber_tlv_builder *builder,
				enum ber_tlv_data_type class,
				enum ber_tlv_data_encoding_type encoding,
				unsigned int new_tag);
gboolean ber_tlv_builder_set_length(struct ber_tlv_builder *builder,
					unsigned int len);
unsigned char *ber_tlv_builder_get_data(struct ber_tlv_builder *builder);
gboolean ber_tlv_builder_recurse(struct ber_tlv_builder *builder,
					struct ber_tlv_builder *recurse);
gboolean ber_tlv_builder_recurse_comprehension(struct ber_tlv_builder *builder,
				struct comprehension_tlv_builder *recurse);
void ber_tlv_builder_optimize(struct ber_tlv_builder *builder,
				unsigned char **pdu, unsigned int *len);

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

void sim_parse_mcc_mnc(const guint8 *bcd, char *mcc, char *mnc);
void sim_encode_mcc_mnc(guint8 *out, const char *mcc, const char *mnc);
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

void sim_extract_bcd_number(const unsigned char *buf, int len, char *out);

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
