/*
 *
 *  RIL library with GLib integration
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012-2013 Canonical Ltd.
 *  Copyright (C) 2013 Jolla Ltd.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <unistd.h>

#include <glib.h>

#include "log.h"
#include "ringbuffer.h"
#include "gril.h"
#include "grilutil.h"

#define RIL_TRACE(ril, fmt, arg...) do {	\
	if (ril->trace == TRUE)			\
		ofono_debug(fmt, ## arg);	\
} while (0)

#define COMMAND_FLAG_EXPECT_PDU			0x1
#define COMMAND_FLAG_EXPECT_SHORT_PROMPT	0x2

#define	RADIO_GID 1001
#define	RADIO_UID 1001

struct ril_request {
	gchar *data;
	guint data_len;
	gint req;
	gint id;
	guint gid;
	GRilResponseFunc callback;
	gpointer user_data;
	GDestroyNotify notify;
};

struct ril_notify_node {
	guint id;
	guint gid;
	GRilNotifyFunc callback;
	gpointer user_data;
	gboolean destroyed;
};

typedef gboolean (*node_remove_func)(struct ril_notify_node *node,
					gpointer user_data);

struct ril_notify {
	GSList *nodes;
};

struct ril_s {
	gint ref_count;				/* Ref count */
	gint next_cmd_id;			/* Next command id */
	guint next_notify_id;			/* Next notify id */
	guint next_gid;				/* Next group id */
	GRilIO *io;				/* GRil IO */
	GQueue *command_queue;			/* Command queue */
	GQueue *out_queue;			/* Commands sent/been sent */
	guint req_bytes_written;		/* bytes written from req */
	GHashTable *notify_list;		/* List of notification reg */
	GRilDisconnectFunc user_disconnect;	/* user disconnect func */
	gpointer user_disconnect_data;		/* user disconnect data */
	guint read_so_far;			/* Number of bytes processed */
	gboolean suspended;			/* Are we suspended? */
	gboolean debug;
	gboolean trace;
	gint timeout_source;
	gboolean destroyed;			/* Re-entrancy guard */
	gboolean in_read_handler;		/* Re-entrancy guard */
	gboolean in_notify;
	enum ofono_ril_vendor vendor;
	int slot;
	GRilMsgIdToStrFunc req_to_string;
	GRilMsgIdToStrFunc unsol_to_string;
};

struct _GRil {
	gint ref_count;
	struct ril_s *parent;
	guint group;
};

struct req_hdr {
	/* Warning: length is stored in network order */
	uint32_t length;
	uint32_t reqid;
	uint32_t serial;
};

#define RIL_PRINT_BUF_SIZE 8096
char print_buf[RIL_PRINT_BUF_SIZE] __attribute__((used));

static void ril_wakeup_writer(struct ril_s *ril);

static const char *request_id_to_string(struct ril_s *ril, int req)
{
	const char *str = NULL;

	if (ril->req_to_string)
		str = ril->req_to_string(req);

	if (str == NULL)
		str = ril_request_id_to_string(req);

	return str;
}

static const char *unsol_request_to_string(struct ril_s *ril, int req)
{
	const char *str = NULL;

	if (ril->unsol_to_string)
		str = ril->unsol_to_string(req);

	if (str == NULL)
		str = ril_unsol_request_to_string(req);

	return str;
}

static void ril_notify_node_destroy(gpointer data, gpointer user_data)
{
	struct ril_notify_node *node = data;
	g_free(node);
}

static void ril_notify_destroy(gpointer user_data)
{
	struct ril_notify *notify = user_data;

	g_slist_foreach(notify->nodes, ril_notify_node_destroy, NULL);
	g_slist_free(notify->nodes);
	g_free(notify);
}

static gint ril_notify_node_compare_by_id(gconstpointer a, gconstpointer b)
{
	const struct ril_notify_node *node = a;
	guint id = GPOINTER_TO_UINT(b);

	if (node->id < id)
		return -1;

	if (node->id > id)
		return 1;

	return 0;
}

static gboolean ril_unregister_all(struct ril_s *ril,
					gboolean mark_only,
					node_remove_func func,
					gpointer userdata)
{
	GHashTableIter iter;
	struct ril_notify *notify;
	struct ril_notify_node *node;
	gpointer key, value;
	GSList *p;
	GSList *c;
	GSList *t;

	if (ril->notify_list == NULL)
		return FALSE;

	g_hash_table_iter_init(&iter, ril->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		notify = value;

		p = NULL;
		c = notify->nodes;

		while (c) {
			node = c->data;

			if (func(node, userdata) != TRUE) {
				p = c;
				c = c->next;
				continue;
			}

			if (mark_only) {
				node->destroyed = TRUE;
				p = c;
				c = c->next;
				continue;
			}

			if (p)
				p->next = c->next;
			else
				notify->nodes = c->next;

			ril_notify_node_destroy(node, NULL);

			t = c;
			c = c->next;
			g_slist_free_1(t);
		}

		if (notify->nodes == NULL)
			g_hash_table_iter_remove(&iter);
	}

	return TRUE;
}

/*
 * This function creates a RIL request.  For a good reference on
 * the layout of RIL requests, responses, and unsolicited requests
 * see:
 *
 * https://wiki.mozilla.org/B2G/RIL
 */
static struct ril_request *ril_request_create(struct ril_s *ril,
						guint gid,
						const gint req,
						const gint id,
						struct parcel *rilp,
						GRilResponseFunc func,
						gpointer user_data,
						GDestroyNotify notify,
						gboolean wakeup)
{
	struct ril_request *r;
	struct req_hdr header;
	guint data_len = 0;

	if (rilp != NULL)
		data_len = rilp->size;

	r = g_try_new0(struct ril_request, 1);
	if (r == NULL) {
		ofono_error("%s Out of memory", __func__);
		return NULL;
	}

	/* Full request size: header size plus buffer length */
	r->data_len = data_len + sizeof(header);

	r->data = g_try_new(char, r->data_len);
	if (r->data == NULL) {
		ofono_error("ril_request: can't allocate new request.");
		g_free(r);
		return NULL;
	}

	/* Length does not include the length field. Network order. */
	header.length = htonl(r->data_len - sizeof(header.length));
	header.reqid = req;
	header.serial = id;

	/* copy header */
	memcpy(r->data, &header, sizeof(header));
	/* copy request data */
	if (data_len)
		memcpy(r->data + sizeof(header), rilp->data, data_len);

	r->req = req;
	r->gid = gid;
	r->id = id;
	r->callback = func;
	r->user_data = user_data;
	r->notify = notify;

	return r;
}

static void ril_request_destroy(struct ril_request *req)
{
	if (req->notify)
		req->notify(req->user_data);

	g_free(req->data);
	g_free(req);
}

static void ril_cleanup(struct ril_s *p)
{
	/* Cleanup pending commands */

	if (p->command_queue) {
		g_queue_free(p->command_queue);
		p->command_queue = NULL;
	}

	if (p->out_queue) {
		g_queue_free(p->out_queue);
		p->out_queue = NULL;
	}

	/* Cleanup registered notifications */
	if (p->notify_list) {
		g_hash_table_destroy(p->notify_list);
		p->notify_list = NULL;
	}

	if (p->timeout_source) {
		g_source_remove(p->timeout_source);
		p->timeout_source = 0;
	}
}

void g_ril_set_disconnect_function(GRil *ril, GRilDisconnectFunc disconnect,
					gpointer user_data)
{
	ril->parent->user_disconnect = disconnect;
	ril->parent->user_disconnect_data = user_data;
}

static void io_disconnect(gpointer user_data)
{
	struct ril_s *ril = user_data;

	ofono_error("%s: disconnected from rild", __func__);

	ril_cleanup(ril);
	g_ril_io_unref(ril->io);
	ril->io = NULL;

	if (ril->user_disconnect)
		ril->user_disconnect(ril->user_disconnect_data);
}

static void handle_response(struct ril_s *p, struct ril_msg *message)
{
	gsize count = g_queue_get_length(p->command_queue);
	struct ril_request *req;
	gboolean found = FALSE;
	guint i, len;
	gint id;

	g_assert(count > 0);

	for (i = 0; i < count; i++) {
		req = g_queue_peek_nth(p->command_queue, i);

		if (req->id == message->serial_no) {
			found = TRUE;
			message->req = req->req;

			if (message->error != RIL_E_SUCCESS)
				RIL_TRACE(p, "[%d,%04d]< %s failed %s",
					p->slot, message->serial_no,
					request_id_to_string(p, message->req),
					ril_error_to_string(message->error));

			req = g_queue_pop_nth(p->command_queue, i);
			if (req->callback)
				req->callback(message, req->user_data);

			len = g_queue_get_length(p->out_queue);

			for (i = 0; i < len; i++) {
				id = GPOINTER_TO_INT(g_queue_peek_nth(
							p->out_queue, i));
				if (id == req->id) {
					g_queue_pop_nth(p->out_queue, i);
					break;
				}
			}

			ril_request_destroy(req);

			if (g_queue_peek_head(p->command_queue))
				ril_wakeup_writer(p);

			break;
		}
	}

	if (found == FALSE)
		ofono_error("No matching request for reply: %s serial_no: %d!",
			request_id_to_string(p, message->req),
			message->serial_no);

}

static gboolean node_check_destroyed(struct ril_notify_node *node,
					gpointer userdata)
{
	gboolean val = GPOINTER_TO_UINT(userdata);

	if (node->destroyed == val)
		return TRUE;

	return FALSE;
}

static void handle_unsol_req(struct ril_s *p, struct ril_msg *message)
{
	struct ril_notify *notify;

	if (p->notify_list == NULL)
		return;

	p->in_notify = TRUE;

	notify = g_hash_table_lookup(p->notify_list, &message->req);
	if (notify != NULL) {
		GSList *list_item;

		for (list_item = notify->nodes; list_item;
				list_item = g_slist_next(list_item)) {
			struct ril_notify_node *node = list_item->data;

			if (node->destroyed)
				continue;

			node->callback(message, node->user_data);
		}
	} else {
		/* Only log events not being listended for... */
		DBG("RIL Event slot %d: %s\n",
			p->slot, unsol_request_to_string(p, message->req));
	}

	p->in_notify = FALSE;

	/* Now destroy nodes possibly removed by callbacks */
	if (notify != NULL)
		ril_unregister_all(p, FALSE, node_check_destroyed,
					GUINT_TO_POINTER(TRUE));
}

static void dispatch(struct ril_s *p, struct ril_msg *message)
{
	int32_t *unsolicited_field, *id_num_field;
	gchar *bufp = message->buf;
	gchar *datap;
	gsize data_len;

	/* This could be done with a struct/union... */
	unsolicited_field = (int32_t *) (void *) bufp;
	if (*unsolicited_field)
		message->unsolicited = TRUE;
	else
		message->unsolicited = FALSE;

	bufp += 4;

	id_num_field = (int32_t *) (void *) bufp;
	if (message->unsolicited) {
		message->req = (int) *id_num_field;

		/*
		 * A RIL Unsolicited Event is two UINT32 fields ( unsolicited,
		 * and req/ev ), so subtract the length of the header from the
		 * overall length to calculate the length of the Event Data.
		 */
		data_len = message->buf_len - 8;
	} else {
		message->serial_no = (int) *id_num_field;

		bufp += 4;
		message->error = *((int32_t *) (void *) bufp);

		/*
		 * A RIL Solicited Response is three UINT32 fields ( unsolicied,
		 * serial_no and error ), so subtract the length of the header
		 * from the overall length to calculate the length of the Event
		 * Data.
		 */
		data_len = message->buf_len - 12;
	}

	/* advance to start of data.. */
	bufp += 4;

	/*
	 * Now, use buffer for event data if present
	 */
	if (data_len) {
		datap = g_try_malloc(data_len);
		if (datap == NULL)
			goto error;

		/* Copy event bytes from message->buf into new buffer */
		memcpy(datap, bufp, data_len);

		/* Free buffer that includes header */
		g_free(message->buf);

		/* ...and replace with new buffer */
		message->buf = datap;
		message->buf_len = data_len;
	} else {
		/* Free buffer that includes header */
		g_free(message->buf);

		/* To know if there was no data when parsing */
		message->buf = NULL;
		message->buf_len = 0;
	}

	if (message->unsolicited == TRUE)
		handle_unsol_req(p, message);
	else
		handle_response(p, message);

error:
	g_free(message->buf);
	g_free(message);
}

static struct ril_msg *read_fixed_record(struct ril_s *p,
						const guchar *bytes, gsize *len)
{
	struct ril_msg *message;
	unsigned message_len, plen;

	/* First four bytes are length in TCP byte order (Big Endian) */
	plen = ntohl(*((uint32_t *) (void *) bytes));
	bytes += 4;

	/*
	 * TODO: Verify that 4k is the max message size from rild.
	 *
	 * These conditions shouldn't happen.  If it does
	 * there are three options:
	 *
	 * 1) ASSERT; ofono will restart via DBus
	 * 2) Consume the bytes & continue
	 * 3) force a disconnect
	 */
	g_assert(plen >= 8 && plen <= 4092);

	/*
	 * If we don't have the whole fixed record in the ringbuffer
	 * then return NULL & leave ringbuffer as is.
	 */

	message_len = *len - 4;
	if (message_len < plen)
		return NULL;

	message = g_try_malloc(sizeof(struct ril_msg));
	g_assert(message != NULL);

	/* allocate ril_msg->buffer */
	message->buf_len = plen;
	message->buf = g_try_malloc(plen);
	g_assert(message->buf != NULL);

	/* Copy bytes into message buffer */
	memmove(message->buf, (const void *) bytes, plen);

	/* Indicate to caller size of record we extracted */
	*len = plen + 4;
	return message;
}

static void new_bytes(struct ring_buffer *rbuf, gpointer user_data)
{
	struct ril_msg *message;
	struct ril_s *p = user_data;
	unsigned int len = ring_buffer_len(rbuf);
	unsigned int wrap = ring_buffer_len_no_wrap(rbuf);
	guchar *buf = ring_buffer_read_ptr(rbuf, p->read_so_far);

	p->in_read_handler = TRUE;

	while (p->suspended == FALSE && (p->read_so_far < len)) {
		gsize rbytes = MIN(len - p->read_so_far, wrap - p->read_so_far);

		if (rbytes < 4) {
			DBG("Not enough bytes for header length: len: %d", len);
			return;
		}

		/*
		 * This function attempts to read the next full length
		 * fixed message from the stream.  if not all bytes are
		 * available, it returns NULL.  otherwise it allocates
		 * and returns a ril_message with the copied bytes, and
		 * drains those bytes from the ring_buffer
		 */
		message = read_fixed_record(p, buf, &rbytes);

		/* wait for the rest of the record... */
		if (message == NULL)
			break;

		buf += rbytes;
		p->read_so_far += rbytes;

		/* TODO: need to better understand how wrap works! */
		if (p->read_so_far == wrap) {
			buf = ring_buffer_read_ptr(rbuf, p->read_so_far);
			wrap = len;
		}

		dispatch(p, message);

		ring_buffer_drain(rbuf, p->read_so_far);

		len -= p->read_so_far;
		wrap -= p->read_so_far;
		p->read_so_far = 0;
	}

	p->in_read_handler = FALSE;

	if (p->destroyed)
		g_free(p);
}

/*
 * This function is a GIOFunc and may be called directly or via an IO watch.
 * The return value controls whether the watch stays active ( TRUE ), or is
 * removed ( FALSE ).
 */
static gboolean can_write_data(gpointer data)
{
	struct ril_s *ril = data;
	struct ril_request *req;
	gsize bytes_written, towrite, len;
	guint qlen, oqlen;
	gint id;
	gboolean written = TRUE;
	guint i, j;

	qlen = g_queue_get_length(ril->command_queue);
	if (qlen < 1)
		return FALSE;

	/* if the whole request was not written */
	if (ril->req_bytes_written != 0) {

		for (i = 0; i < qlen; i++) {
			req = g_queue_peek_nth(ril->command_queue, i);
			if (req) {
				id = GPOINTER_TO_INT(g_queue_peek_head(
							ril->out_queue));
				if (req->id == id)
					goto out;
			} else {
				return FALSE;
			}
		}
	}
	/* if no requests already sent */
	oqlen = g_queue_get_length(ril->out_queue);
	if (oqlen < 1) {
		req = g_queue_peek_head(ril->command_queue);
		if (req == NULL)
			return FALSE;

		g_queue_push_head(ril->out_queue, GINT_TO_POINTER(req->id));

		goto out;
	}

	for (i = 0; i < qlen; i++) {
		req = g_queue_peek_nth(ril->command_queue, i);
		if (req == NULL)
			return FALSE;

		for (j = 0; j < oqlen; j++) {
			id = GPOINTER_TO_INT(
					g_queue_peek_nth(ril->out_queue, j));
			if (req->id == id) {
				written = TRUE;
				break;
			} else {
				written = FALSE;
			}
		}

		if (written == FALSE)
			break;
	}

	/* watcher fired though requests already written */
	if (written == TRUE)
		return FALSE;

	g_queue_push_head(ril->out_queue, GINT_TO_POINTER(req->id));

out:
	len = req->data_len;

	towrite = len - ril->req_bytes_written;

#ifdef WRITE_SCHEDULER_DEBUG
	if (towrite > 5)
		towrite = 5;
#endif

	bytes_written = g_ril_io_write(ril->io,
					req->data + ril->req_bytes_written,
					towrite);

	if (bytes_written == 0)
		return FALSE;

	ril->req_bytes_written += bytes_written;
	if (bytes_written < towrite)
		return TRUE;
	else
		ril->req_bytes_written = 0;

	return FALSE;
}

static void ril_wakeup_writer(struct ril_s *ril)
{
	g_ril_io_set_write_handler(ril->io, can_write_data, ril);
}

static void ril_suspend(struct ril_s *ril)
{
	ril->suspended = TRUE;

	g_ril_io_set_write_handler(ril->io, NULL, NULL);
	g_ril_io_set_read_handler(ril->io, NULL, NULL);
	g_ril_io_set_debug(ril->io, NULL, NULL);
}

static gboolean ril_set_debug(struct ril_s *ril,
				GRilDebugFunc func, gpointer user_data)
{
	if (ril->io == NULL)
		return FALSE;

	g_ril_io_set_debug(ril->io, func, user_data);

	return TRUE;
}

static void ril_unref(struct ril_s *ril)
{
	gboolean is_zero;

	is_zero = g_atomic_int_dec_and_test(&ril->ref_count);

	if (is_zero == FALSE)
		return;

	if (ril->io) {
		ril_suspend(ril);
		g_ril_io_unref(ril->io);
		ril->io = NULL;
		ril_cleanup(ril);
	}

	if (ril->in_read_handler)
		ril->destroyed = TRUE;
	else
		g_free(ril);
}

static gboolean node_compare_by_group(struct ril_notify_node *node,
					gpointer userdata)
{
	guint group = GPOINTER_TO_UINT(userdata);

	if (node->gid == group)
		return TRUE;

	return FALSE;
}

static void set_process_id(gid_t gid, uid_t uid)
{
	if (setegid(gid) < 0)
		ofono_error("%s: setegid(%d) failed: %s (%d)",
				__func__, gid, strerror(errno), errno);

	if (seteuid(uid) < 0)
		ofono_error("%s: seteuid(%d) failed: %s (%d)",
				__func__, uid, strerror(errno), errno);
}

static struct ril_s *create_ril(const char *sock_path)

{
	struct ril_s *ril;
	struct sockaddr_un addr;
	int sk;
	GIOChannel *io;

	ril = g_try_new0(struct ril_s, 1);
	if (ril == NULL)
		return ril;

	ril->ref_count = 1;
	ril->next_cmd_id = 1;
	ril->next_notify_id = 1;
	ril->next_gid = 0;
	ril->req_bytes_written = 0;
	ril->trace = FALSE;

	/* sock_path is allowed to be NULL for unit tests */
	if (sock_path == NULL)
		return ril;

	sk = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sk < 0) {
		ofono_error("create_ril: can't create unix socket: %s (%d)\n",
				strerror(errno), errno);
		goto error;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	/* RIL expects user radio to connect to the socket */
	set_process_id(RADIO_GID, RADIO_UID);

	if (connect(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		ofono_error("create_ril: can't connect to RILD: %s (%d)\n",
				strerror(errno), errno);
		/* Switch back to root */
		set_process_id(0, 0);
		goto error;
	}

	/* Switch back to root */
	set_process_id(0, 0);

	io = g_io_channel_unix_new(sk);
	if (io == NULL) {
		ofono_error("create_ril: can't open RILD io channel: %s (%d)\n",
				strerror(errno), errno);
		goto error;
	}

	g_io_channel_set_close_on_unref(io, TRUE);
	g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK, NULL);

	ril->io = g_ril_io_new(io);
	if (ril->io == NULL) {
		ofono_error("create_ril: can't create ril->io");
		goto error;
	}

	g_ril_io_set_disconnect_function(ril->io, io_disconnect, ril);

	ril->command_queue = g_queue_new();
	if (ril->command_queue == NULL) {
		ofono_error("create_ril: Couldn't create command_queue.");
		goto error;
	}

	ril->out_queue = g_queue_new();
	if (ril->out_queue == NULL) {
		ofono_error("create_ril: Couldn't create out_queue.");
		goto error;
	}

	ril->notify_list = g_hash_table_new_full(g_int_hash, g_int_equal,
							g_free,
							ril_notify_destroy);

	g_ril_io_set_read_handler(ril->io, new_bytes, ril);

	return ril;

error:
	ril_unref(ril);

	return NULL;
}

static struct ril_notify *ril_notify_create(struct ril_s *ril,
						const int req)
{
	struct ril_notify *notify;
	int *key;

	notify = g_try_new0(struct ril_notify, 1);
	if (notify == NULL)
		return 0;

	key = g_try_new0(int, 1);
	if (key == NULL)
		return 0;

	*key = req;

	g_hash_table_insert(ril->notify_list, key, notify);

	return notify;
}

static void ril_cancel_group(struct ril_s *ril, guint group)
{
	int n = 0;
	guint len, i;
	struct ril_request *req;
	gboolean sent;

	if (ril->command_queue == NULL)
		return;

	while ((req = g_queue_peek_nth(ril->command_queue, n)) != NULL) {
		if (req->id == 0 || req->gid != group) {
			n += 1;
			continue;
		}

		req->callback = NULL;
		sent = FALSE;

		len = g_queue_get_length(ril->out_queue);
		for (i = 0; i < len; i++) {
			if (GPOINTER_TO_INT(
					g_queue_peek_nth(ril->out_queue, i))
					== req->id) {
				n += 1;
				sent = TRUE;
				break;
			}
		 }

		if (sent)
			continue;

		g_queue_remove(ril->command_queue, req);
		ril_request_destroy(req);
	}
}

static guint ril_register(struct ril_s *ril, guint group,
				const int req, GRilNotifyFunc func,
				gpointer user_data)
{
	struct ril_notify *notify;
	struct ril_notify_node *node;

	if (ril->notify_list == NULL)
		return 0;

	if (func == NULL)
		return 0;

	notify = g_hash_table_lookup(ril->notify_list, &req);

	if (notify == NULL)
		notify = ril_notify_create(ril, req);

	if (notify == NULL)
		return 0;

	node = g_try_new0(struct ril_notify_node, 1);
	if (node == NULL)
		return 0;

	node->id = ril->next_notify_id++;
	node->gid = group;
	node->callback = func;
	node->user_data = user_data;

	notify->nodes = g_slist_prepend(notify->nodes, node);

	return node->id;
}

static gboolean ril_unregister(struct ril_s *ril, gboolean mark_only,
					guint group, guint id)
{
	GHashTableIter iter;
	struct ril_notify *notify;
	struct ril_notify_node *node;
	gpointer key, value;
	GSList *l;

	if (ril->notify_list == NULL)
		return FALSE;

	g_hash_table_iter_init(&iter, ril->notify_list);

	while (g_hash_table_iter_next(&iter, &key, &value)) {
		notify = value;

		l = g_slist_find_custom(notify->nodes, GUINT_TO_POINTER(id),
					ril_notify_node_compare_by_id);

		if (l == NULL)
			continue;

		node = l->data;

		if (node->gid != group)
			return FALSE;

		if (mark_only) {
			node->destroyed = TRUE;
			return TRUE;
		}

		ril_notify_node_destroy(node, NULL);
		notify->nodes = g_slist_remove(notify->nodes, node);

		if (notify->nodes == NULL)
			g_hash_table_iter_remove(&iter);

		return TRUE;
	}

	return FALSE;
}

void g_ril_init_parcel(const struct ril_msg *message, struct parcel *rilp)
{
	/* Set up Parcel struct for proper parsing */
	rilp->data = message->buf;
	rilp->size = message->buf_len;
	rilp->capacity = message->buf_len;
	rilp->offset = 0;
	rilp->malformed = 0;
}

GRil *g_ril_new(const char *sock_path, enum ofono_ril_vendor vendor)
{
	GRil *ril;

	ril = g_try_new0(GRil, 1);
	if (ril == NULL)
		return NULL;

	ril->parent = create_ril(sock_path);
	if (ril->parent == NULL) {
		g_free(ril);
		return NULL;
	}

	ril->group = ril->parent->next_gid++;
	ril->ref_count = 1;

	ril->parent->vendor = vendor;

	return ril;
}

GRil *g_ril_clone(GRil *clone)
{
	GRil *ril;

	if (clone == NULL)
		return NULL;

	ril = g_try_new0(GRil, 1);
	if (ril == NULL)
		return NULL;

	ril->parent = clone->parent;
	ril->group = ril->parent->next_gid++;
	ril->ref_count = 1;
	g_atomic_int_inc(&ril->parent->ref_count);

	return ril;
}

GIOChannel *g_ril_get_channel(GRil *ril)
{
	if (ril == NULL || ril->parent->io == NULL)
		return NULL;

	return g_ril_io_get_channel(ril->parent->io);

}

GRilIO *g_ril_get_io(GRil *ril)
{
	if (ril == NULL)
		return NULL;

	return ril->parent->io;
}

GRil *g_ril_ref(GRil *ril)
{
	if (ril == NULL)
		return NULL;

	g_atomic_int_inc(&ril->ref_count);

	return ril;
}

gint g_ril_send(GRil *ril, const gint reqid, struct parcel *rilp,
		GRilResponseFunc func, gpointer user_data,
		GDestroyNotify notify)
{
	struct ril_request *r;
	struct ril_s *p;

	if (ril == NULL
		|| ril->parent == NULL
		|| ril->parent->command_queue == NULL)
			return 0;

	p = ril->parent;

	r = ril_request_create(p, ril->group, reqid, p->next_cmd_id, rilp,
				func, user_data, notify, FALSE);

	if (rilp != NULL)
		parcel_free(rilp);

	if (r == NULL)
		return 0;

	p->next_cmd_id++;

	g_queue_push_tail(p->command_queue, r);

	ril_wakeup_writer(p);

	if (rilp == NULL)
		g_ril_print_request_no_args(ril, r->id, reqid);
	else
		g_ril_print_request(ril, r->id, reqid);

	return r->id;
}

void g_ril_unref(GRil *ril)
{
	gboolean is_zero;

	if (ril == NULL)
		return;

	is_zero = g_atomic_int_dec_and_test(&ril->ref_count);

	if (is_zero == FALSE)
		return;

	ril_cancel_group(ril->parent, ril->group);
	g_ril_unregister_all(ril);
	ril_unref(ril->parent);

	g_free(ril);
}

gboolean g_ril_get_trace(GRil *ril)
{

	if (ril == NULL || ril->parent == NULL)
		return FALSE;

	return ril->parent->trace;
}

gboolean g_ril_set_trace(GRil *ril, gboolean trace)
{

	if (ril == NULL || ril->parent == NULL)
		return FALSE;

	return ril->parent->trace = trace;
}

gboolean g_ril_set_slot(GRil *ril, int slot)
{
	if (ril == NULL || ril->parent == NULL)
		return FALSE;

	ril->parent->slot = slot;
	return TRUE;
}

int g_ril_get_slot(GRil *ril)
{
	if (ril == NULL)
		return 0;

	return ril->parent->slot;
}

gboolean g_ril_set_debugf(GRil *ril,
			GRilDebugFunc func, gpointer user_data)
{

	if (ril == NULL || ril->group != 0)
		return FALSE;

	return ril_set_debug(ril->parent, func, user_data);
}

gboolean g_ril_set_vendor_print_msg_id_funcs(GRil *ril,
					GRilMsgIdToStrFunc req_to_string,
					GRilMsgIdToStrFunc unsol_to_string)
{
	if (ril == NULL || ril->parent == NULL)
		return FALSE;

	ril->parent->req_to_string = req_to_string;
	ril->parent->unsol_to_string = unsol_to_string;

	return TRUE;
}

guint g_ril_register(GRil *ril, const int req,
			GRilNotifyFunc func, gpointer user_data)
{
	if (ril == NULL)
		return 0;

	return ril_register(ril->parent, ril->group, req,
				func, user_data);
}

gboolean g_ril_unregister(GRil *ril, guint id)
{
	if (ril == NULL)
		return FALSE;

	return ril_unregister(ril->parent, ril->parent->in_notify,
					ril->group, id);
}

gboolean g_ril_unregister_all(GRil *ril)
{
	if (ril == NULL)
		return FALSE;

	return ril_unregister_all(ril->parent,
					ril->parent->in_notify,
					node_compare_by_group,
					GUINT_TO_POINTER(ril->group));
}

enum ofono_ril_vendor g_ril_vendor(GRil *ril)
{
	if (ril == NULL)
		return OFONO_RIL_VENDOR_AOSP;

	return ril->parent->vendor;
}

const char *g_ril_request_id_to_string(GRil *ril, int req)
{
	return request_id_to_string(ril->parent, req);
}

const char *g_ril_unsol_request_to_string(GRil *ril, int req)
{
	return unsol_request_to_string(ril->parent, req);
}
