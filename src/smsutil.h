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

#define CBS_MAX_GSM_CHARS 93
#define SMS_MSGID_LEN 20

enum sms_type {
	SMS_TYPE_DELIVER = 0,
	SMS_TYPE_DELIVER_REPORT_ACK,
	SMS_TYPE_DELIVER_REPORT_ERROR,
	SMS_TYPE_STATUS_REPORT,
	SMS_TYPE_SUBMIT,
	SMS_TYPE_SUBMIT_REPORT_ACK,
	SMS_TYPE_SUBMIT_REPORT_ERROR,
	SMS_TYPE_COMMAND
};

/* 23.040 Section 9.1.2.5 */
enum sms_number_type {
	SMS_NUMBER_TYPE_UNKNOWN = 0,
	SMS_NUMBER_TYPE_INTERNATIONAL = 1,
	SMS_NUMBER_TYPE_NATIONAL = 2,
	SMS_NUMBER_TYPE_NETWORK_SPECIFIC = 3,
	SMS_NUMBER_TYPE_SUBSCRIBER = 4,
	SMS_NUMBER_TYPE_ALPHANUMERIC = 5,
	SMS_NUMBER_TYPE_ABBREVIATED = 6,
	SMS_NUMBER_TYPE_RESERVED = 7
};

/* 23.040 Section 9.1.2.5 */
enum sms_numbering_plan {
	SMS_NUMBERING_PLAN_UNKNOWN = 0,
	SMS_NUMBERING_PLAN_ISDN = 1,
	SMS_NUMBERING_PLAN_DATA = 3,
	SMS_NUMBERING_PLAN_TELEX = 4,
	SMS_NUMBERING_PLAN_SC1 = 5,
	SMS_NUMBERING_PLAN_SC2 = 6,
	SMS_NUMBERING_PLAN_NATIONAL = 8,
	SMS_NUMBERING_PLAN_PRIVATE = 9,
	SMS_NUMBERING_PLAN_ERMES = 10,
	SMS_NUMBERING_PLAN_RESERVED = 15
};

enum sms_validity_period_format {
	SMS_VALIDITY_PERIOD_FORMAT_ABSENT = 0,
	SMS_VALIDITY_PERIOD_FORMAT_ENHANCED = 1,
	SMS_VALIDITY_PERIOD_FORMAT_RELATIVE = 2,
	SMS_VALIDITY_PERIOD_FORMAT_ABSOLUTE = 3,
};

enum sms_st {
	SMS_ST_COMPLETED_RECEIVED = 0x0,
	SMS_ST_COMPLETED_UNABLE_TO_CONFIRM = 0x1,
	SMS_ST_COMPLETED_REPLACED = 0x2,
	SMS_ST_COMPLETED_LAST = 0x1F,
	SMS_ST_TEMPORARY_CONGESTION = 0x20,
	SMS_ST_TEMPORARY_SME_BUSY = 0x21,
	SMS_ST_TEMPORARY_NO_RESPONSE = 0x22,
	SMS_ST_TEMPORARY_SERVICE_REJECTED = 0x23,
	SMS_ST_TEMPORARY_QOS_UNAVAILABLE = 0x24,
	SMS_ST_TEMPORARY_SME_ERROR = 0x25,
	SMS_ST_TEMPORARY_LAST = 0x2F,
	SMS_ST_PERMANENT_RP_ERROR = 0x40,
	SMS_ST_PERMANENT_INVALID_DESTINATION = 0x41,
	SMS_ST_PERMANENT_CONNECTION_REJECTED = 0x42,
	SMS_ST_PERMANENT_NOT_OBTAINABLE = 0x43,
	SMS_ST_PERMANENT_QOS_UNAVAILABLE = 0x44,
	SMS_ST_PERMANENT_INTERWORKING_UNAVAILABLE = 0x45,
	SMS_ST_PERMANENT_VALIDITY_PERIOD_EXPIRED = 0x46,
	SMS_ST_PERMANENT_DELETED = 0x47,
	SMS_ST_PERMANENT_SC_ADMIN_DELETED = 0x48,
	SMS_ST_PERMANENT_SM_DOES_NOT_EXIST = 0x49,
	SMS_ST_PERMANENT_LAST = 0x4F,
	SMS_ST_TEMPFINAL_CONGESTION = 0x60,
	SMS_ST_TEMPFINAL_SME_BUSY = 0x61,
	SMS_ST_TEMPFINAL_NO_RESPONSE = 0x62,
	SMS_ST_TEMPFINAL_SERVICE_REJECTED = 0x63,
	SMS_ST_TEMPFINAL_QOS_UNAVAILABLE = 0x64,
	SMS_ST_TEMPFINAL_SME_ERROR = 0x65,
	SMS_ST_TEMPFINAL_LAST = 0x6F,
};

enum sms_ct {
	SMS_CT_ENQUIRY = 0,
	SMS_CT_CANCEL_SRR = 1,
	SMS_CT_DELETE_SM = 2,
	SMS_CT_ENABLE_SRR = 3
};

enum sms_iei {
	SMS_IEI_CONCATENATED_8BIT = 0x00,
	SMS_IEI_SPECIAL_MESSAGE_INDICATION = 0x01,
	SMS_IEI_APPLICATION_ADDRESS_8BIT = 0x04,
	SMS_IEI_APPLICATION_ADDRESS_16BIT = 0x05,
	SMS_IEI_SMSC_CONTROL_PARAMETERS = 0x06,
	SMS_IEI_UDH_SOURCE_INDICATOR = 0x07,
	SMS_IEI_CONCATENATED_16BIT = 0x08,
	SMS_IEI_WCMP = 0x09,
	SMS_IEI_TEXT_FORMAT = 0x0A,
	SMS_IEI_PREDEFINED_SOUND = 0x0B,
	SMS_IEI_USER_DEFINED_SOUND = 0x0C,
	SMS_IEI_PREDEFINED_ANIMATION = 0x0D,
	SMS_IEI_LARGE_ANIMATION = 0x0E,
	SMS_IEI_SMALL_ANIMATION = 0x0F,
	SMS_IEI_LARGE_PICTURE = 0x10,
	SMS_IEI_SMALL_PICTURE = 0x11,
	SMS_IEI_VARIABLE_PICTURE = 0x12,
	SMS_IEI_USER_PROMPT_INDICATOR = 0x13,
	SMS_IEI_EXTENDED_OBJECT = 0x14,
	SMS_IEI_REUSED_EXTENDED_OBJECT = 0x15,
	SMS_IEI_COMPRESSION_CONTROL = 0x16,
	SMS_IEI_OBJECT_DISTRIBUTION_INDICATOR = 0x17,
	SMS_IEI_STANDARD_WVG_OBJECT = 0x18,
	SMS_IEI_CHARACTER_SIZE_WVG_OBJECT = 0x19,
	SMS_IEI_EXTENDED_OBJECT_DATA_REQUEST_COMMAND = 0x1A,
	SMS_IEI_RFC822_EMAIL_HEADER = 0x20,
	SMS_IEI_HYPERLINK_ELEMENT = 0x21,
	SMS_IEI_REPLY_ADDRESS_ELEMENT = 0x22,
	SMS_IEI_ENHANCED_VOICE_MAIL_INFORMATION = 0x23,
	SMS_IEI_NATIONAL_LANGUAGE_SINGLE_SHIFT = 0x24,
	SMS_IEI_NATIONAL_LANGUAGE_LOCKING_SHIFT = 0x25,
	SMS_IEI_INVALID = 0xFFF
};

enum sms_class {
	SMS_CLASS_0 = 0,
	SMS_CLASS_1 = 1,
	SMS_CLASS_2 = 2,
	SMS_CLASS_3 = 3,
	SMS_CLASS_UNSPECIFIED = 4,
};

enum sms_charset {
	SMS_CHARSET_7BIT = 0,
	SMS_CHARSET_8BIT = 1,
	SMS_CHARSET_UCS2 = 2,
};

enum sms_alphabet {
	SMS_ALPHABET_DEFAULT = 0,
	SMS_ALPHABET_TURKISH,
	SMS_ALPHABET_SPANISH,
	SMS_ALPHABET_PORTUGUESE,
};

enum sms_mwi_type {
	SMS_MWI_TYPE_VOICE = 0,
	SMS_MWI_TYPE_FAX = 1,
	SMS_MWI_TYPE_EMAIL = 2,
	SMS_MWI_TYPE_OTHER = 3,
	SMS_MWI_TYPE_VIDEO = 4,
};

enum sms_pid_type {
	SMS_PID_TYPE_SM_TYPE_0 = 0x40,
	SMS_PID_TYPE_REPLACE_SM_TYPE_1 = 0x41,
	SMS_PID_TYPE_REPLACE_SM_TYPE_2 = 0x42,
	SMS_PID_TYPE_REPLACE_SM_TYPE_3 = 0x43,
	SMS_PID_TYPE_REPLACE_SM_TYPE_4 = 0x44,
	SMS_PID_TYPE_REPLACE_SM_TYPE_5 = 0x45,
	SMS_PID_TYPE_REPLACE_SM_TYPE_6 = 0x46,
	SMS_PID_TYPE_REPLACE_SM_TYPE_7 = 0x47,
	SMS_PID_TYPE_ENHANCED_MESSAGE_SERVICE = 0x5e,
	SMS_PID_TYPE_RETURN_CALL = 0x5f,
	SMS_PID_TYPE_ANSI136 = 0x7c,
	SMS_PID_TYPE_ME_DOWNLOAD = 0x7d,
	SMS_PID_TYPE_ME_DEPERSONALIZATION = 0x7e,
	SMS_PID_TYPE_USIM_DOWNLOAD = 0x7f,
};

enum cbs_language {
	CBS_LANGUAGE_GERMAN = 0x0,
	CBS_LANGUAGE_ENGLISH = 0x1,
	CBS_LANGUAGE_ITALIAN = 0x2,
	CBS_LANGUAGE_FRENCH = 0x3,
	CBS_LANGUAGE_SPANISH = 0x4,
	CBS_LANGUAGE_DUTCH = 0x5,
	CBS_LANGUAGE_SWEDISH = 0x6,
	CBS_LANGUAGE_DANISH = 0x7,
	CBS_LANGUAGE_PORTUGESE = 0x8,
	CBS_LANGUAGE_FINNISH = 0x9,
	CBS_LANGUAGE_NORWEGIAN = 0xA,
	CBS_LANGUAGE_GREEK = 0xB,
	CBS_LANGUAGE_TURKISH = 0xC,
	CBS_LANGUAGE_HUNGARIAN = 0xD,
	CBS_LANGUAGE_POLISH = 0xE,
	CBS_LANGUAGE_UNSPECIFIED = 0xF,
	CBS_LANGUAGE_CZECH = 0x20,
	CBS_LANGUAGE_HEBREW = 0x21,
	CBS_LANGUAGE_ARABIC = 0x22,
	CBS_LANGUAGE_RUSSIAN = 0x23,
	CBS_LANGUAGE_ICELANDIC = 0x24
};

enum cbs_geo_scope {
	CBS_GEO_SCOPE_CELL_IMMEDIATE,
	CBS_GEO_SCOPE_PLMN,
	CBS_GEO_SCOPE_SERVICE_AREA,
	CBS_GEO_SCOPE_CELL_NORMAL
};

struct sms_address {
	enum sms_number_type number_type;
	enum sms_numbering_plan numbering_plan;
	char address[21]; /* Max 20 in semi-octet, 11 in alnum */
};

struct sms_scts {
	guint8 year;
	guint8 month;
	guint8 day;
	guint8 hour;
	guint8 minute;
	guint8 second;
	gboolean has_timezone;
	gint8 timezone;
};

struct sms_validity_period {
	union {
		guint8 relative;
		struct sms_scts absolute;
		guint8 enhanced[7];
	};
};

struct sms_deliver {
	gboolean mms;
	gboolean sri;
	gboolean udhi;
	gboolean rp;
	struct sms_address oaddr;
	guint8 pid;
	guint8 dcs;
	struct sms_scts scts;
	guint8 udl;
	guint8 ud[140];
};

struct sms_deliver_err_report {
	gboolean udhi;
	guint8 fcs;
	guint8 pi;
	guint8 pid;
	guint8 dcs;
	guint8 udl;
	guint8 ud[158];
};

struct sms_deliver_ack_report {
	gboolean udhi;
	guint8 pi;
	guint8 pid;
	guint8 dcs;
	guint8 udl;
	guint8 ud[159];
};

struct sms_command {
	gboolean udhi;
	gboolean srr;
	guint8 mr;
	guint8 pid;
	enum sms_ct ct;
	guint8 mn;
	struct sms_address daddr;
	guint8 cdl;
	guint8 cd[156];
};

struct sms_status_report {
	gboolean udhi;
	gboolean mms;
	gboolean srq;
	guint8 mr;
	struct sms_address raddr;
	struct sms_scts scts;
	struct sms_scts dt;
	enum sms_st st;
	guint8 pi;
	guint8 pid;
	guint8 dcs;
	guint8 udl;
	guint8 ud[143];
};

struct sms_submit {
	gboolean rd;
	enum sms_validity_period_format vpf;
	gboolean rp;
	gboolean udhi;
	gboolean srr;
	guint8 mr;
	struct sms_address daddr;
	guint8 pid;
	guint8 dcs;
	struct sms_validity_period vp;
	guint8 udl;
	guint8 ud[140];
};

struct sms_submit_ack_report {
	gboolean udhi;
	guint8 pi;
	struct sms_scts scts;
	guint8 pid;
	guint8 dcs;
	guint8 udl;
	guint8 ud[152];
};

struct sms_submit_err_report {
	gboolean udhi;
	guint8 fcs;
	guint8 pi;
	struct sms_scts scts;
	guint8 pid;
	guint8 dcs;
	guint8 udl;
	guint8 ud[151];
};

struct sms {
	struct sms_address sc_addr;
	enum sms_type type;
	union {
		struct sms_deliver deliver;
		struct sms_deliver_ack_report deliver_ack_report;
		struct sms_deliver_err_report deliver_err_report;
		struct sms_submit submit;
		struct sms_submit_ack_report submit_ack_report;
		struct sms_submit_err_report submit_err_report;
		struct sms_command command;
		struct sms_status_report status_report;
	};
};

struct sms_udh_iter {
	const guint8 *data;
	guint8 offset;
};

struct sms_assembly_node {
	struct sms_address addr;
	time_t ts;
	GSList *fragment_list;
	guint16 ref;
	guint8 max_fragments;
	guint8 num_fragments;
	unsigned int bitmap[8];
};

struct sms_assembly {
	const char *imsi;
	GSList *assembly_list;
};

struct id_table_node {
	unsigned int mrs[8];
	time_t expiration;
	unsigned char total_mrs;
	unsigned char sent_mrs;
	gboolean deliverable;
} __attribute__((packed));

struct status_report_assembly {
	const char *imsi;
	GHashTable *assembly_table;
};

struct cbs {
	enum cbs_geo_scope gs;			/* 2 bits */
	guint16 message_code;			/* 10 bits */
	guint8 update_number;			/* 4 bits */
	guint16 message_identifier;		/* 16 bits */
	guint8 dcs;				/* 8 bits */
	guint8 max_pages;			/* 4 bits */
	guint8 page;				/* 4 bits */
	guint8 ud[82];
};

struct cbs_assembly_node {
	guint32 serial;
	guint16 bitmap;
	GSList *pages;
};

struct cbs_assembly {
	GSList *assembly_list;
	GSList *recv_plmn;
	GSList *recv_loc;
	GSList *recv_cell;
};

struct cbs_topic_range {
	unsigned short min;
	unsigned short max;
};

struct txq_backup_entry {
	GSList *msg_list;
	unsigned char uuid[SMS_MSGID_LEN];
	unsigned long flags;
};

static inline gboolean is_bit_set(unsigned char oct, int bit)
{
	int mask = 1 << bit;
	return oct & mask ? TRUE : FALSE;
}

static inline unsigned char bit_field(unsigned char oct, int start, int num)
{
	unsigned char mask = (1 << num) - 1;

	return (oct >> start) & mask;
}

void extract_bcd_number(const unsigned char *buf, int len, char *out);
void encode_bcd_number(const char *number, unsigned char *out);

gboolean sms_decode(const unsigned char *pdu, int len, gboolean outgoing,
			int tpdu_len, struct sms *out);

gboolean sms_decode_unpacked_stk_pdu(const unsigned char *pdu, int len,
					struct sms *out);

gboolean sms_encode(const struct sms *in, int *len, int *tpdu_len,
			unsigned char *pdu);

/*
 * Length is based on the address being 12 hex characters plus a
 * terminating NUL char. See sms_assembly_extract_address().
 */
#define DECLARE_SMS_ADDR_STR(a) char a[25]

gboolean sms_decode_address_field(const unsigned char *pdu, int len,
					int *offset, gboolean sc,
					struct sms_address *out);

gboolean sms_encode_address_field(const struct sms_address *in, gboolean sc,
					unsigned char *pdu, int *offset);

guint8 sms_decode_semi_octet(guint8 in);

gboolean sms_decode_scts(const unsigned char *pdu, int len,
				int *offset, struct sms_scts *out);

gboolean sms_encode_scts(const struct sms_scts *in, unsigned char *pdu,
				int *offset);

int sms_udl_in_bytes(guint8 ud_len, guint8 dcs);

time_t sms_scts_to_time(const struct sms_scts *scts, struct tm *remote);

const char *sms_address_to_string(const struct sms_address *addr);
void sms_address_from_string(struct sms_address *addr, const char *str);

const guint8 *sms_extract_common(const struct sms *sms, gboolean *out_udhi,
					guint8 *out_dcs, guint8 *out_udl,
					guint8 *out_max);

gboolean sms_udh_iter_init(const struct sms *sms, struct sms_udh_iter *iter);
gboolean sms_udh_iter_init_from_cbs(const struct cbs *cbs,
					struct sms_udh_iter *iter);
guint8 sms_udh_iter_get_udh_length(struct sms_udh_iter *iter);
const guint8 *sms_udh_iter_get_ud_after_header(struct sms_udh_iter *iter);
enum sms_iei sms_udh_iter_get_ie_type(struct sms_udh_iter *iter);
guint8 sms_udh_iter_get_ie_length(struct sms_udh_iter *iter);
void sms_udh_iter_get_ie_data(struct sms_udh_iter *iter, guint8 *data);
gboolean sms_udh_iter_has_next(struct sms_udh_iter *iter);
gboolean sms_udh_iter_next(struct sms_udh_iter *iter);

gboolean sms_dcs_decode(guint8 dcs, enum sms_class *cls,
			enum sms_charset *charset,
			gboolean *compressed, gboolean *autodelete);

gboolean sms_mwi_dcs_decode(guint8 dcs, enum sms_mwi_type *type,
				enum sms_charset *charset,
				gboolean *active, gboolean *discard);

gboolean sms_extract_app_port(const struct sms *sms, int *dst, int *src,
				gboolean *is_8bit);
gboolean sms_extract_concatenation(const struct sms *sms, guint16 *ref_num,
					guint8 *max_msgs, guint8 *seq_num);
gboolean sms_extract_language_variant(const struct sms *sms, guint8 *locking,
					guint8 *single);

unsigned char *sms_decode_datagram(GSList *sms_list, long *out_len);
char *sms_decode_text(GSList *sms_list);

struct sms_assembly *sms_assembly_new(const char *imsi);
void sms_assembly_free(struct sms_assembly *assembly);
GSList *sms_assembly_add_fragment(struct sms_assembly *assembly,
					const struct sms *sms, time_t ts,
					const struct sms_address *addr,
					guint16 ref, guint8 max, guint8 seq);
void sms_assembly_expire(struct sms_assembly *assembly, time_t before);
gboolean sms_address_to_hex_string(const struct sms_address *in, char *straddr);

struct status_report_assembly *status_report_assembly_new(const char *imsi);
void status_report_assembly_free(struct status_report_assembly *assembly);
gboolean status_report_assembly_report(struct status_report_assembly *assembly,
					const struct sms *status_report,
					unsigned char *out_msgid,
					gboolean *msg_delivered);
void status_report_assembly_add_fragment(struct status_report_assembly
					*assembly, const unsigned char *msgid,
					const struct sms_address *to,
					unsigned char mr, time_t expiration,
					unsigned char total_mrs);
void status_report_assembly_expire(struct status_report_assembly *assembly,
					time_t before);

gboolean sms_tx_backup_store(const char *imsi, unsigned long id,
				unsigned long flags, const char *uuid,
				guint8 seq, const unsigned char *pdu,
				int pdu_len, int tpdu_len);
void sms_tx_backup_remove(const char *imsi, unsigned long id,
				unsigned long flags, const char *uuid,
				guint8 seq);
void sms_tx_backup_free(const char *imsi, unsigned long id,
				unsigned long flags, const char *uuid);
GQueue *sms_tx_queue_load(const char *imsi);

GSList *sms_text_prepare(const char *to, const char *utf8, guint16 ref,
				gboolean use_16bit,
				gboolean use_delivery_reports);

GSList *sms_text_prepare_with_alphabet(const char *to, const char *utf8,
				guint16 ref, gboolean use_16bit,
				gboolean use_delivery_reports,
				enum sms_alphabet alphabet);

GSList *sms_datagram_prepare(const char *to,
				const unsigned char *data, unsigned int len,
				guint16 ref, gboolean use_16bit_ref,
				unsigned short src, unsigned short dst,
				gboolean use_16bit_port,
				gboolean use_delivery_reports);

gboolean cbs_dcs_decode(guint8 dcs, gboolean *udhi, enum sms_class *cls,
			enum sms_charset *charset, gboolean *compressed,
			enum cbs_language *language, gboolean *iso639);

gboolean iso639_2_from_language(enum cbs_language lang, char *iso639);
gboolean cbs_decode(const unsigned char *pdu, int len, struct cbs *out);
gboolean cbs_encode(const struct cbs *cbs, int *len, unsigned char *pdu);
gboolean cbs_extract_app_port(const struct cbs *cbs, int *dst, int *src,
				gboolean *is_8bit);

char *cbs_decode_text(GSList *cbs_list, char *iso639_lang);

struct cbs_assembly *cbs_assembly_new(void);
void cbs_assembly_free(struct cbs_assembly *assembly);
GSList *cbs_assembly_add_page(struct cbs_assembly *assembly,
				const struct cbs *cbs);
void cbs_assembly_location_changed(struct cbs_assembly *assembly, gboolean plmn,
					gboolean lac, gboolean ci);

char *cbs_topic_ranges_to_string(GSList *ranges);
GSList *cbs_extract_topic_ranges(const char *ranges);
GSList *cbs_optimize_ranges(GSList *ranges);
gboolean cbs_topic_in_range(unsigned int topic, GSList *ranges);

char *ussd_decode(int dcs, int len, const unsigned char *data);
gboolean ussd_encode(const char *str, long *items_written, unsigned char *pdu);
