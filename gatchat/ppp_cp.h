/*
 *
 *  PPP library with GLib integration
 *
 *  Copyright (C) 2009-2010  Intel Corporation. All rights reserved.
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

struct pppcp_data;
struct ppp_option_iter;

/* option format */
struct ppp_option {
	guint8 type;
	guint8 length;
	guint8 data[0];
};

enum rcr_result {
	RCR_ACCEPT,
	RCR_REJECT,
	RCR_NAK,
};

enum pppcp_code {
	PPPCP_CODE_TYPE_CONFIGURE_REQUEST = 1,
	PPPCP_CODE_TYPE_CONFIGURE_ACK,
	PPPCP_CODE_TYPE_CONFIGURE_NAK,
	PPPCP_CODE_TYPE_CONFIGURE_REJECT,
	PPPCP_CODE_TYPE_TERMINATE_REQUEST,
	PPPCP_CODE_TYPE_TERMINATE_ACK,
	PPPCP_CODE_TYPE_CODE_REJECT,
	PPPCP_CODE_TYPE_PROTOCOL_REJECT,
	PPPCP_CODE_TYPE_ECHO_REQUEST,
	PPPCP_CODE_TYPE_ECHO_REPLY,
	PPPCP_CODE_TYPE_DISCARD_REQUEST
};

struct pppcp_packet {
	guint8 code;
	guint8 identifier;
	guint16 length;
	guint8 data[0];
} __attribute__((packed));

struct ppp_option_iter {
	guint16 max;
	guint16 pos;
	const guint8 *pdata;
	guint8 type;
	guint8 len;
	const guint8 *option_data;
};

struct pppcp_proto {
	guint16 proto;
	const char *name;
	guint16 supported_codes;
	void (*this_layer_up)(struct pppcp_data *data);
	void (*this_layer_down)(struct pppcp_data *data);
	void (*this_layer_started)(struct pppcp_data *data);
	void (*this_layer_finished)(struct pppcp_data *data);
	/* Remote side acked these options, we can now use them */
	void (*rca)(struct pppcp_data *pppcp, const struct pppcp_packet *pkt);
	/*
	 * Remote side sent us an Conf-Req-Nak or Conf-Req-Rej.  The protocol
	 * driver should examine the packet and update its options accordingly,
	 * then use set_local_options to set a new set of options to try
	 * before returning
	 */
	void (*rcn_nak)(struct pppcp_data *pppcp,
					const struct pppcp_packet *pkt);
	void (*rcn_rej)(struct pppcp_data *pppcp,
					const struct pppcp_packet *pkt);
	/*
	 * Remote side has sent us a request with its options, return whether
	 * we should ack / nak / rej these options.  In the case of nak / rej,
	 * the list of options to be sent to the peer is given in the
	 * new_options & new_len out arguments
	 */
	enum rcr_result (*rcr)(struct pppcp_data *pppcp,
					const struct pppcp_packet *pkt,
					guint8 **new_options, guint16 *new_len);
};

void ppp_option_iter_init(struct ppp_option_iter *iter,
					const struct pppcp_packet *packet);
gboolean ppp_option_iter_next(struct ppp_option_iter *iter);
guint8 ppp_option_iter_get_type(struct ppp_option_iter *iter);
guint8 ppp_option_iter_get_length(struct ppp_option_iter *iter);
const guint8 *ppp_option_iter_get_data(struct ppp_option_iter *iter);

struct pppcp_data *pppcp_new(GAtPPP *ppp, const struct pppcp_proto *proto,
				gboolean dormant, guint max_failure);
void pppcp_free(struct pppcp_data *data);

void pppcp_set_data(struct pppcp_data *pppcp, gpointer data);
gpointer pppcp_get_data(struct pppcp_data *pppcp);
GAtPPP *pppcp_get_ppp(struct pppcp_data *pppcp);

guint8 pppcp_get_code(const guint8 *data);

void pppcp_set_local_options(struct pppcp_data *data,
				const guint8 *options,
				guint16 len);

void pppcp_process_packet(gpointer priv, const guint8 *new_packet, gsize len);
void pppcp_send_protocol_reject(struct pppcp_data *data,
				const guint8 *rejected_packet, gsize len);
void pppcp_signal_open(struct pppcp_data *data);
void pppcp_signal_close(struct pppcp_data *data);
void pppcp_signal_up(struct pppcp_data *data);
void pppcp_signal_down(struct pppcp_data *data);
