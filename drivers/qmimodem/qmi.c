/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#define _GNU_SOURCE
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "qmi.h"
#include "ctl.h"

typedef void (*qmi_message_func_t)(uint16_t message, uint16_t length,
					const void *buffer, void *user_data);

struct qmi_device {
	int ref_count;
	int fd;
	GIOChannel *io;
	bool close_on_unref;
	guint read_watch;
	guint write_watch;
	GQueue *req_queue;
	GQueue *control_queue;
	GQueue *service_queue;
	uint8_t next_control_tid;
	uint16_t next_service_tid;
	qmi_debug_func_t debug_func;
	void *debug_data;
	uint16_t control_major;
	uint16_t control_minor;
	char *version_str;
	struct qmi_version *version_list;
	uint8_t version_count;
	GHashTable *service_list;
	unsigned int release_users;
};

struct qmi_service {
	int ref_count;
	struct qmi_device *device;
	bool shared;
	uint8_t type;
	uint16_t major;
	uint16_t minor;
	uint8_t client_id;
	uint16_t next_notify_id;
	GList *notify_list;
};

struct qmi_param {
	void *data;
	uint16_t length;
};

struct qmi_result {
	uint16_t message;
	uint16_t result;
	uint16_t error;
	const void *data;
	uint16_t length;
};

struct qmi_request {
	uint16_t tid;
	uint8_t client;
	void *buf;
	size_t len;
	qmi_message_func_t callback;
	void *user_data;
};

struct qmi_notify {
	uint16_t id;
	uint16_t message;
	qmi_result_func_t callback;
	void *user_data;
	qmi_destroy_func_t destroy;
};

struct qmi_mux_hdr {
	uint8_t  frame;		/* Always 0x01 */
	uint16_t length;	/* Packet size without frame byte */
	uint8_t  flags;		/* Either 0x00 or 0x80 */
	uint8_t  service;	/* Service type (0x00 for control) */
	uint8_t  client;	/* Client identifier (0x00 for control) */
} __attribute__ ((packed));
#define QMI_MUX_HDR_SIZE 6

struct qmi_control_hdr {
	uint8_t  type;		/* Bit 1 = response, Bit 2 = indication */
	uint8_t  transaction;	/* Transaction identifier */
} __attribute__ ((packed));
#define QMI_CONTROL_HDR_SIZE 2

struct qmi_service_hdr {
	uint8_t  type;		/* Bit 2 = response, Bit 3 = indication */
	uint16_t transaction;	/* Transaction identifier */
} __attribute__ ((packed));
#define QMI_SERVICE_HDR_SIZE 3

struct qmi_message_hdr {
	uint16_t message;	/* Message identifier */
	uint16_t length;	/* Message size without header */
	uint8_t data[0];
} __attribute__ ((packed));
#define QMI_MESSAGE_HDR_SIZE 4

struct qmi_tlv_hdr {
	uint8_t type;
	uint16_t length;
	uint8_t value[0];
} __attribute__ ((packed));
#define QMI_TLV_HDR_SIZE 3

void qmi_free(void *ptr)
{
	free(ptr);
}

static struct qmi_request *__request_alloc(uint8_t service,
				uint8_t client, uint16_t message,
				uint16_t headroom, const void *data,
				uint16_t length, qmi_message_func_t func,
				void *user_data, void **head)
{
	struct qmi_request *req;
	struct qmi_mux_hdr *hdr;
	struct qmi_message_hdr *msg;

	req = g_try_new0(struct qmi_request, 1);
	if (!req)
		return NULL;

	req->len = QMI_MUX_HDR_SIZE + headroom + QMI_MESSAGE_HDR_SIZE + length;

	req->buf = g_try_malloc(req->len);
	if (!req->buf) {
		g_free(req);
		return NULL;
	}

	req->client = client;

	hdr = req->buf;

	hdr->frame = 0x01;
	hdr->length = GUINT16_TO_LE(req->len - 1);
	hdr->flags = 0x00;
	hdr->service = service;
	hdr->client = client;

	msg = req->buf + QMI_MUX_HDR_SIZE + headroom;

	msg->message = GUINT16_TO_LE(message);
	msg->length = GUINT16_TO_LE(length);

	if (data && length > 0)
		memcpy(req->buf + QMI_MUX_HDR_SIZE + headroom +
					QMI_MESSAGE_HDR_SIZE, data, length);

	req->callback = func;
	req->user_data = user_data;

	*head = req->buf + QMI_MUX_HDR_SIZE;

	return req;
}

static void __request_free(gpointer data, gpointer user_data)
{
	struct qmi_request *req = data;

	g_free(req->buf);
	g_free(req);
}

static gint __request_compare(gconstpointer a, gconstpointer b)
{
	const struct qmi_request *req = a;
	uint16_t tid = GPOINTER_TO_UINT(b);

	return req->tid - tid;
}

static void __notify_free(gpointer data, gpointer user_data)
{
	struct qmi_notify *notify = data;

	if (notify->destroy)
		notify->destroy(notify->user_data);

	g_free(notify);
}

static gint __notify_compare(gconstpointer a, gconstpointer b)
{
	const struct qmi_notify *notify = a;
	uint16_t id = GPOINTER_TO_UINT(b);

	return notify->id - id;
}

static gboolean __service_compare_shared(gpointer key, gpointer value,
							gpointer user_data)
{
	struct qmi_service *service = value;
	uint8_t type = GPOINTER_TO_UINT(user_data);

	if (!service->shared)
		return FALSE;

	if (service->type == type)
		return TRUE;

	return FALSE;
}

static void __hexdump(const char dir, const unsigned char *buf, size_t len,
				qmi_debug_func_t function, void *user_data)
{
	static const char hexdigits[] = "0123456789abcdef";
	char str[68];
	size_t i;

	if (!function || !len)
		return;

	str[0] = dir;

	for (i = 0; i < len; i++) {
		str[((i % 16) * 3) + 1] = ' ';
		str[((i % 16) * 3) + 2] = hexdigits[buf[i] >> 4];
		str[((i % 16) * 3) + 3] = hexdigits[buf[i] & 0xf];
		str[(i % 16) + 51] = isprint(buf[i]) ? buf[i] : '.';

		if ((i + 1) % 16 == 0) {
			str[49] = ' ';
			str[50] = ' ';
			str[67] = '\0';
			function(str, user_data);
			str[0] = ' ';
		}
	}

	if (i % 16 > 0) {
		size_t j;
		for (j = (i % 16); j < 16; j++) {
			str[(j * 3) + 1] = ' ';
			str[(j * 3) + 2] = ' ';
			str[(j * 3) + 3] = ' ';
			str[j + 51] = ' ';
		}
		str[49] = ' ';
		str[50] = ' ';
		str[67] = '\0';
		function(str, user_data);
	}
}

static const char *__service_type_to_string(uint8_t type)
{
	switch (type) {
	case QMI_SERVICE_CONTROL:
		return "CTL";
	case QMI_SERVICE_WDS:
		return "WDS";
	case QMI_SERVICE_DMS:
		return "DMS";
	case QMI_SERVICE_NAS:
		return "NAS";
	case QMI_SERVICE_QOS:
		return "QOS";
	case QMI_SERVICE_WMS:
		return "WMS";
	case QMI_SERVICE_PDS:
		return "PDS";
	case QMI_SERVICE_AUTH:
		return "AUTH";
	case QMI_SERVICE_AT:
		return "AT";
	case QMI_SERVICE_VOICE:
		return "VOICE";
	case QMI_SERVICE_CAT:
		return "CAT";
	case QMI_SERVICE_UIM:
		return "UIM";
	case QMI_SERVICE_PBM:
		return "PBM";
	case QMI_SERVICE_RMTFS:
		return "RMTFS";
	case QMI_SERVICE_LOC:
		return "LOC";
	case QMI_SERVICE_SAR:
		return "SAR";
	case QMI_SERVICE_CSD:
		return "CSD";
	case QMI_SERVICE_EFS:
		return "EFS";
	case QMI_SERVICE_TS:
		return "TS";
	case QMI_SERVICE_TMD:
		return "TMS";
	case QMI_SERVICE_CAT_OLD:
		return "CAT";
	case QMI_SERVICE_RMS:
		return "RMS";
	case QMI_SERVICE_OMA:
		return "OMA";
	}

	return NULL;
}

static void __debug_msg(const char dir, const void *buf, size_t len,
				qmi_debug_func_t function, void *user_data)
{
	const struct qmi_mux_hdr *hdr;
	const struct qmi_message_hdr *msg;
	const char *service;
	const void *ptr;
	uint16_t offset;
	char strbuf[72 + 16], *str;

	if (!function || !len)
		return;

	hdr = buf;

	str = strbuf;
	service = __service_type_to_string(hdr->service);
	if (service)
		str += sprintf(str, "%c   %s", dir, service);
	else
		str += sprintf(str, "%c   %d", dir, hdr->service);

	if (hdr->service == QMI_SERVICE_CONTROL) {
		const struct qmi_control_hdr *ctl;
		const char *type;

		ctl = buf + QMI_MUX_HDR_SIZE;
		msg = buf + QMI_MUX_HDR_SIZE + QMI_CONTROL_HDR_SIZE;
		ptr = buf + QMI_MUX_HDR_SIZE + QMI_CONTROL_HDR_SIZE +
							QMI_MESSAGE_HDR_SIZE;

		switch (ctl->type) {
		case 0x00:
			type = "_req";
			break;
		case 0x01:
			type = "_resp";
			break;
		case 0x02:
			type = "_ind";
			break;
		default:
			type = "";
			break;
		}

		str += sprintf(str, "%s msg=%d len=%d", type,
					GUINT16_FROM_LE(msg->message),
					GUINT16_FROM_LE(msg->length));

		str += sprintf(str, " [client=%d,type=%d,tid=%d,len=%d]",
					hdr->client, ctl->type,
					ctl->transaction,
					GUINT16_FROM_LE(hdr->length));
	} else {
		const struct qmi_service_hdr *srv;
		const char *type;

		srv = buf + QMI_MUX_HDR_SIZE;
		msg = buf + QMI_MUX_HDR_SIZE + QMI_SERVICE_HDR_SIZE;
		ptr = buf + QMI_MUX_HDR_SIZE + QMI_SERVICE_HDR_SIZE +
							QMI_MESSAGE_HDR_SIZE;

		switch (srv->type) {
		case 0x00:
			type = "_req";
			break;
		case 0x02:
			type = "_resp";
			break;
		case 0x04:
			type = "_ind";
			break;
		default:
			type = "";
			break;
		}

		str += sprintf(str, "%s msg=%d len=%d", type,
					GUINT16_FROM_LE(msg->message),
					GUINT16_FROM_LE(msg->length));

		str += sprintf(str, " [client=%d,type=%d,tid=%d,len=%d]",
					hdr->client, srv->type,
					GUINT16_FROM_LE(srv->transaction),
					GUINT16_FROM_LE(hdr->length));
	}

	function(strbuf, user_data);

	if (!msg->length)
		return;

	str = strbuf;
	str += sprintf(str, "      ");
	offset = 0;

	while (offset + QMI_TLV_HDR_SIZE < GUINT16_FROM_LE(msg->length)) {
		const struct qmi_tlv_hdr *tlv = ptr + offset;
		uint16_t tlv_length = GUINT16_FROM_LE(tlv->length);

		if (tlv->type == 0x02 && tlv_length == QMI_RESULT_CODE_SIZE) {
			const struct qmi_result_code *result = ptr + offset +
							QMI_TLV_HDR_SIZE;

			str += sprintf(str, " {type=%d,error=%d}", tlv->type,
						GUINT16_FROM_LE(result->error));
		} else {
			str += sprintf(str, " {type=%d,len=%d}", tlv->type,
								tlv_length);
		}

		if (str - strbuf > 72) {
			function(strbuf, user_data);

			str = strbuf;
			str += sprintf(str, "      ");
		}

		offset += QMI_TLV_HDR_SIZE + tlv_length;
	}

	function(strbuf, user_data);
}

static void __debug_device(struct qmi_device *device,
					const char *format, ...)
{
	char strbuf[72 + 16];
	va_list ap;

	if (!device->debug_func)
		return;

	va_start(ap, format);
	vsnprintf(strbuf, sizeof(strbuf), format, ap);
	va_end(ap);

	device->debug_func(strbuf, device->debug_data);
}

static gboolean can_write_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct qmi_device *device = user_data;
	struct qmi_mux_hdr *hdr;
	struct qmi_request *req;
	ssize_t bytes_written;

	req = g_queue_pop_head(device->req_queue);
	if (!req)
		return FALSE;

	bytes_written = write(device->fd, req->buf, req->len);
	if (bytes_written < 0)
		return FALSE;

	__hexdump('>', req->buf, bytes_written,
				device->debug_func, device->debug_data);

	__debug_msg(' ', req->buf, bytes_written,
				device->debug_func, device->debug_data);

	hdr = req->buf;

	if (hdr->service == QMI_SERVICE_CONTROL)
		g_queue_push_tail(device->control_queue, req);
	else
		g_queue_push_tail(device->service_queue, req);

	g_free(req->buf);
	req->buf = NULL;

	if (g_queue_get_length(device->req_queue) > 0)
		return TRUE;

	return FALSE;
}

static void write_watch_destroy(gpointer user_data)
{
	struct qmi_device *device = user_data;

	device->write_watch = 0;
}

static void wakeup_writer(struct qmi_device *device)
{
	if (device->write_watch > 0)
		return;

	device->write_watch = g_io_add_watch_full(device->io, G_PRIORITY_HIGH,
				G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				can_write_data, device, write_watch_destroy);
}

static void __request_submit(struct qmi_device *device,
				struct qmi_request *req, uint16_t transaction)
{
	req->tid = transaction;

	g_queue_push_tail(device->req_queue, req);

	wakeup_writer(device);
}

static void service_notify(gpointer key, gpointer value, gpointer user_data)
{
	struct qmi_service *service = value;
	struct qmi_result *result = user_data;
	GList *list;

	for (list = g_list_first(service->notify_list); list;
						list = g_list_next(list)) {
		struct qmi_notify *notify = list->data;

		if (notify->message == result->message)
			notify->callback(result, notify->user_data);
	}
}

static void handle_indication(struct qmi_device *device,
			uint8_t service_type, uint8_t client_id,
			uint16_t message, uint16_t length, const void *data)
{
	struct qmi_service *service;
	struct qmi_result result;
	unsigned int hash_id;

	if (service_type == QMI_SERVICE_CONTROL)
		return;

	result.result = 0;
	result.error = 0;
	result.message = message;
	result.data = data;
	result.length = length;

	if (client_id == 0xff) {
		g_hash_table_foreach(device->service_list,
						service_notify, &result);
		return;
	}

	hash_id = service_type | (client_id << 8);

	service = g_hash_table_lookup(device->service_list,
					GUINT_TO_POINTER(hash_id));
	if (!service)
		return;

	service_notify(NULL, service, &result);
}

static void handle_packet(struct qmi_device *device,
				const struct qmi_mux_hdr *hdr, const void *buf)
{
	struct qmi_request *req;
	uint16_t message, length;
	const void *data;

	if (hdr->service == QMI_SERVICE_CONTROL) {
		const struct qmi_control_hdr *control = buf;
		const struct qmi_message_hdr *msg;
		unsigned int tid;
		GList *list;

		/* Ignore control messages with client identifier */
		if (hdr->client != 0x00)
			return;

		msg = buf + QMI_CONTROL_HDR_SIZE;

		message = GUINT16_FROM_LE(msg->message);
		length = GUINT16_FROM_LE(msg->length);

		data = buf + QMI_CONTROL_HDR_SIZE + QMI_MESSAGE_HDR_SIZE;

		tid = control->transaction;

		if (control->type == 0x02 && control->transaction == 0x00) {
			handle_indication(device, hdr->service, hdr->client,
							message, length, data);
			return;
		}

		list = g_queue_find_custom(device->control_queue,
				GUINT_TO_POINTER(tid), __request_compare);
		if (!list)
			return;

		req = list->data;

		g_queue_delete_link(device->control_queue, list);
	} else {
		const struct qmi_service_hdr *service = buf;
		const struct qmi_message_hdr *msg;
		unsigned int tid;
		GList *list;

		msg = buf + QMI_SERVICE_HDR_SIZE;

		message = GUINT16_FROM_LE(msg->message);
		length = GUINT16_FROM_LE(msg->length);

		data = buf + QMI_SERVICE_HDR_SIZE + QMI_MESSAGE_HDR_SIZE;

		tid = GUINT16_FROM_LE(service->transaction);

		if (service->type == 0x04 && tid == 0x0000) {
			handle_indication(device, hdr->service, hdr->client,
							message, length, data);
			return;
		}

		list = g_queue_find_custom(device->service_queue,
				GUINT_TO_POINTER(tid), __request_compare);
		if (!list)
			return;

		req = list->data;

		g_queue_delete_link(device->service_queue, list);
	}

	if (req->callback)
		req->callback(message, length, data, req->user_data);

	__request_free(req, NULL);
}

static gboolean received_data(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct qmi_device *device = user_data;
	struct qmi_mux_hdr *hdr;
	unsigned char buf[2048];
	ssize_t bytes_read;
	uint16_t offset;

	if (cond & G_IO_NVAL)
		return FALSE;

	bytes_read = read(device->fd, buf, sizeof(buf));
	if (bytes_read < 0)
		return TRUE;

	__hexdump('<', buf, bytes_read,
				device->debug_func, device->debug_data);

	offset = 0;

	while (offset < bytes_read) {
		uint16_t len;

		/* Check if QMI mux header fits into packet */
		if (bytes_read - offset < QMI_MUX_HDR_SIZE)
			break;

		hdr = (void *) (buf + offset);

		/* Check for fixed frame and flags value */
		if (hdr->frame != 0x01 || hdr->flags != 0x80)
			break;

		len = GUINT16_FROM_LE(hdr->length) + 1;

		/* Check that packet size matches frame size */
		if (bytes_read - offset < len)
			break;

		__debug_msg(' ', buf + offset, len,
				device->debug_func, device->debug_data);

		handle_packet(device, hdr, buf + offset + QMI_MUX_HDR_SIZE);

		offset += len;
	}

	return TRUE;
}

static void read_watch_destroy(gpointer user_data)
{
	struct qmi_device *device = user_data;

	device->read_watch = 0;
}

static void service_destroy(gpointer data)
{
	struct qmi_service *service = data;

	if (!service->device)
		return;

	service->device = NULL;
}

struct qmi_device *qmi_device_new(int fd)
{
	struct qmi_device *device;
	long flags;

	device = g_try_new0(struct qmi_device, 1);
	if (!device)
		return NULL;

	__debug_device(device, "device %p new", device);

	device->ref_count = 1;

	device->fd = fd;
	device->close_on_unref = false;

	flags = fcntl(device->fd, F_GETFL, NULL);
	if (flags < 0) {
		g_free(device);
		return NULL;
	}

	if (!(flags & O_NONBLOCK)) {
		if (fcntl(device->fd, F_SETFL, flags | O_NONBLOCK) < 0) {
			g_free(device);
			return NULL;
		}
	}

	device->io = g_io_channel_unix_new(device->fd);

	g_io_channel_set_encoding(device->io, NULL, NULL);
	g_io_channel_set_buffered(device->io, FALSE);

	device->read_watch = g_io_add_watch_full(device->io, G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				received_data, device, read_watch_destroy);

	g_io_channel_unref(device->io);

	device->req_queue = g_queue_new();
	device->control_queue = g_queue_new();
	device->service_queue = g_queue_new();

	device->service_list = g_hash_table_new_full(g_direct_hash,
					g_direct_equal, NULL, service_destroy);

	return device;
}

struct qmi_device *qmi_device_ref(struct qmi_device *device)
{
	if (!device)
		return NULL;

	__sync_fetch_and_add(&device->ref_count, 1);

	return device;
}

void qmi_device_unref(struct qmi_device *device)
{
	if (!device)
		return;

	if (__sync_sub_and_fetch(&device->ref_count, 1))
		return;

	__debug_device(device, "device %p free", device);

	g_queue_foreach(device->control_queue, __request_free, NULL);
	g_queue_free(device->control_queue);

	g_queue_foreach(device->service_queue, __request_free, NULL);
	g_queue_free(device->service_queue);

	g_queue_foreach(device->req_queue, __request_free, NULL);
	g_queue_free(device->req_queue);

	if (device->write_watch > 0)
		g_source_remove(device->write_watch);

	if (device->read_watch > 0)
		g_source_remove(device->read_watch);

	if (device->close_on_unref)
		close(device->fd);

	g_hash_table_destroy(device->service_list);

	g_free(device->version_str);
	g_free(device->version_list);

	g_free(device);
}

void qmi_device_set_debug(struct qmi_device *device,
				qmi_debug_func_t func, void *user_data)
{
	if (device == NULL)
		return;

	device->debug_func = func;
	device->debug_data = user_data;
}

void qmi_device_set_close_on_unref(struct qmi_device *device, bool do_close)
{
	if (!device)
		return;

	device->close_on_unref = do_close;
}

static const void *tlv_get(const void *data, uint16_t size,
					uint8_t type, uint16_t *length)
{
	const void *ptr = data;
	uint16_t len = size;

	while (len > QMI_TLV_HDR_SIZE) {
		const struct qmi_tlv_hdr *tlv = ptr;
		uint16_t tlv_length = GUINT16_FROM_LE(tlv->length);

		if (tlv->type == type) {
			if (length)
				*length = tlv_length;

			return ptr + QMI_TLV_HDR_SIZE;
		}

		ptr += QMI_TLV_HDR_SIZE + tlv_length;
		len -= QMI_TLV_HDR_SIZE + tlv_length;
	}

	return NULL;
}

struct discover_data {
	struct qmi_device *device;
	qmi_discover_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
	guint timeout;
};

static void discover_callback(uint16_t message, uint16_t length,
					const void *buffer, void *user_data)
{
	struct discover_data *data = user_data;
	struct qmi_device *device = data->device;
	const struct qmi_result_code *result_code;
	const struct qmi_service_list *service_list;
	const void *ptr;
	uint16_t len;
	struct qmi_version *list;
	uint8_t count;
	unsigned int i;

	g_source_remove(data->timeout);

	count = 0;
	list = NULL;

	result_code = tlv_get(buffer, length, 0x02, &len);
	if (!result_code)
		goto done;

	if (len != QMI_RESULT_CODE_SIZE)
		goto done;

	service_list = tlv_get(buffer, length, 0x01, &len);
	if (!service_list)
		goto done;

	if (len < QMI_SERVICE_LIST_SIZE)
		goto done;

	list = g_try_malloc(sizeof(struct qmi_version) * service_list->count);
	if (!list)
		goto done;

	for (i = 0; i < service_list->count; i++) {
		uint16_t major =
			GUINT16_FROM_LE(service_list->services[i].major);
		uint16_t minor =
			GUINT16_FROM_LE(service_list->services[i].minor);
		uint8_t type = service_list->services[i].type;

		if (type == QMI_SERVICE_CONTROL) {
			device->control_major = major;
			device->control_minor = minor;
			continue;
		}

		list[count].type = type;
		list[count].major = major;
		list[count].minor = minor;
		list[count].name = __service_type_to_string(type);

		count++;
	}

	ptr = tlv_get(buffer, length, 0x10, &len);
	if (!ptr)
		goto done;

	device->version_str = strndup(ptr + 1, *((uint8_t *) ptr));

	service_list = ptr + *((uint8_t *) ptr) + 1;

	for (i = 0; i < service_list->count; i++) {
		if (service_list->services[i].type == QMI_SERVICE_CONTROL)
			continue;
	}

done:
	device->version_list = list;
	device->version_count = count;

	if (data->func)
		data->func(count, list, data->user_data);

	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);
}

static gboolean discover_reply(gpointer user_data)
{
	struct discover_data *data = user_data;
	struct qmi_device *device = data->device;

	data->timeout = 0;

	if (data->func)
		data->func(device->version_count,
				device->version_list, data->user_data);

	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);

	return FALSE;
}

bool qmi_device_discover(struct qmi_device *device, qmi_discover_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct discover_data *data;
	struct qmi_request *req;
	struct qmi_control_hdr *hdr;

	if (!device)
		return false;

	__debug_device(device, "device %p discover", device);

	data = g_try_new0(struct discover_data, 1);
	if (!data)
		return false;

	data->device = device;
	data->func = func;
	data->user_data = user_data;
	data->destroy = destroy;

	if (device->version_list) {
		g_timeout_add_seconds(0, discover_reply, data);
		return true;
	}

	req = __request_alloc(QMI_SERVICE_CONTROL, 0x00,
			QMI_CTL_GET_VERSION_INFO, QMI_CONTROL_HDR_SIZE,
			NULL, 0, discover_callback, data, (void **) &hdr);
	if (!req) {
		g_free(data);
		return false;
	}

	if (device->next_control_tid < 1)
		device->next_control_tid = 1;

	hdr->type = 0x00;
	hdr->transaction = device->next_control_tid++;

	__request_submit(device, req, hdr->transaction);

	data->timeout = g_timeout_add_seconds(5, discover_reply, data);

	return true;
}

static void release_client(struct qmi_device *device,
				uint8_t type, uint8_t client_id,
				qmi_message_func_t func, void *user_data)
{
	unsigned char release_req[] = { 0x01, 0x02, 0x00, type, client_id };
	struct qmi_request *req;
	struct qmi_control_hdr *hdr;

	req = __request_alloc(QMI_SERVICE_CONTROL, 0x00,
			QMI_CTL_RELEASE_CLIENT_ID, QMI_CONTROL_HDR_SIZE,
			release_req, sizeof(release_req),
			func, user_data, (void **) &hdr);
	if (!req) {
		func(0x0000, 0x0000, NULL, user_data);
		return;
	}

	if (device->next_control_tid < 1)
		device->next_control_tid = 1;

	hdr->type = 0x00;
	hdr->transaction = device->next_control_tid++;

	__request_submit(device, req, hdr->transaction);
}

struct shutdown_data {
	struct qmi_device *device;
	qmi_shutdown_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
};

static gboolean shutdown_reply(gpointer user_data)
{
	struct shutdown_data *data = user_data;

	if (data->func)
		data->func(data->user_data);

	g_free(data);

	return FALSE;
}

static gboolean shutdown_timeout(gpointer user_data)
{
	struct shutdown_data *data = user_data;
	struct qmi_device *device = data->device;

	if (device->release_users > 0)
		return TRUE;

	return shutdown_reply(data);
}

bool qmi_device_shutdown(struct qmi_device *device, qmi_shutdown_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct shutdown_data *data;

	if (!device)
		return false;

	__debug_device(device, "device %p shutdown", device);

	data = g_try_new0(struct shutdown_data, 1);
	if (!data)
		return false;

	data->device = device;
	data->func = func;
	data->user_data = user_data;
	data->destroy = destroy;

	if (device->release_users > 0)
		g_timeout_add_seconds(0, shutdown_timeout, data);
	else
		g_timeout_add_seconds(0, shutdown_reply, data);

	return true;
}

struct qmi_param *qmi_param_new(void)
{
	struct qmi_param *param;

	param = g_try_new0(struct qmi_param, 1);
	if (!param)
		return NULL;

	return param;
}

void qmi_param_free(struct qmi_param *param)
{
	if (!param)
		return;

	g_free(param->data);
	g_free(param);
}

bool qmi_param_append(struct qmi_param *param, uint8_t type,
					uint16_t length, const void *data)
{
	struct qmi_tlv_hdr *tlv;
	void *ptr;

	if (!param || !type)
		return false;

	if (!length)
		return true;

	if (!data)
		return false;

	if (param->data)
		ptr = g_try_realloc(param->data,
				param->length + QMI_TLV_HDR_SIZE + length);
	else
		ptr = g_try_malloc(QMI_TLV_HDR_SIZE + length);

	if (!ptr)
		return false;

	tlv = ptr + param->length;

	tlv->type = type;
	tlv->length = GUINT16_TO_LE(length);
	memcpy(tlv->value, data, length);

	param->data = ptr;
	param->length += QMI_TLV_HDR_SIZE + length;

	return true;
}

bool qmi_param_append_uint8(struct qmi_param *param, uint8_t type,
							uint8_t value)
{
	unsigned char buf[1] = { value };

	return qmi_param_append(param, type, sizeof(buf), buf);
}

bool qmi_param_append_uint16(struct qmi_param *param, uint8_t type,
							uint16_t value)
{
	unsigned char buf[2] = { value & 0xff, (value & 0xff00) >> 8 };

	return qmi_param_append(param, type, sizeof(buf), buf);
}

bool qmi_param_append_uint32(struct qmi_param *param, uint8_t type,
							uint32_t value)
{
	unsigned char buf[4] = { value & 0xff, (value & 0xff00) >> 8,
					(value & 0xff0000) >> 16,
					(value & 0xff000000) >> 24 };

	return qmi_param_append(param, type, sizeof(buf), buf);
}

struct qmi_param *qmi_param_new_uint8(uint8_t type, uint8_t value)
{
	struct qmi_param *param;

	param = qmi_param_new();
	if (!param)
		return NULL;

	if (!qmi_param_append_uint8(param, type, value)) {
		qmi_param_free(param);
		return NULL;
	}

	return param;
}

struct qmi_param *qmi_param_new_uint16(uint8_t type, uint16_t value)
{
	struct qmi_param *param;

	param = qmi_param_new();
	if (!param)
		return NULL;

	if (!qmi_param_append_uint16(param, type, value)) {
		qmi_param_free(param);
		return NULL;
	}

	return param;
}

struct qmi_param *qmi_param_new_uint32(uint8_t type, uint32_t value)
{
	struct qmi_param *param;

	param = qmi_param_new();
	if (!param)
		return NULL;

	if (!qmi_param_append_uint32(param, type, value)) {
		qmi_param_free(param);
		return NULL;
	}

	return param;
}

bool qmi_result_set_error(struct qmi_result *result, uint16_t *error)
{
	if (!result) {
		if (error)
			*error = 0xffff;
		return true;
	}

	if (result->result == 0x0000)
		return false;

	if (error)
		*error = result->error;

	return true;
}

const void *qmi_result_get(struct qmi_result *result, uint8_t type,
							uint16_t *length)
{
	if (!result || !type)
		return NULL;

	return tlv_get(result->data, result->length, type, length);
}

char *qmi_result_get_string(struct qmi_result *result, uint8_t type)
{
	const void *ptr;
	uint16_t len;

	if (!result || !type)
		return NULL;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return NULL;

	return strndup(ptr, len);
}

bool qmi_result_get_uint8(struct qmi_result *result, uint8_t type,
							uint8_t *value)
{
	const unsigned char *ptr;
	uint16_t len;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	if (value)
		*value = *ptr;

	return true;
}

bool qmi_result_get_uint16(struct qmi_result *result, uint8_t type,
							uint16_t *value)
{
	const unsigned char *ptr;
	uint16_t len, tmp;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	memcpy(&tmp, ptr, 2);

	if (value)
		*value = GUINT16_FROM_LE(tmp);

	return true;
}

bool qmi_result_get_uint32(struct qmi_result *result, uint8_t type,
							uint32_t *value)
{
	const unsigned char *ptr;
	uint16_t len;
	uint32_t tmp;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	memcpy(&tmp, ptr, 4);

	if (value)
		*value = GUINT32_FROM_LE(tmp);

	return true;
}

bool qmi_result_get_uint64(struct qmi_result *result, uint8_t type,
							uint64_t *value)
{
	const unsigned char *ptr;
	uint16_t len;
	uint64_t tmp;

	if (!result || !type)
		return false;

	ptr = tlv_get(result->data, result->length, type, &len);
	if (!ptr)
		return false;

	memcpy(&tmp, ptr, 8);

	if (value)
		*value = GUINT64_FROM_LE(tmp);

	return true;
}

struct service_create_data {
	struct qmi_device *device;
	bool shared;
	uint8_t type;
	uint16_t major;
	uint16_t minor;
	qmi_create_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
	guint timeout;
};

static gboolean service_create_reply(gpointer user_data)
{
	struct service_create_data *data = user_data;

	data->func(NULL, data->user_data);

	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);

	return FALSE;
}

static void service_create_callback(uint16_t message, uint16_t length,
					const void *buffer, void *user_data)
{
	struct service_create_data *data = user_data;
	struct qmi_device *device = data->device;
	struct qmi_service *service = NULL;
	const struct qmi_result_code *result_code;
	const struct qmi_client_id *client_id;
	uint16_t len;
	unsigned int hash_id;

	g_source_remove(data->timeout);

	result_code = tlv_get(buffer, length, 0x02, &len);
	if (!result_code)
		goto done;

	if (len != QMI_RESULT_CODE_SIZE)
		goto done;

	client_id = tlv_get(buffer, length, 0x01, &len);
	if (!client_id)
		goto done;

	if (len != QMI_CLIENT_ID_SIZE)
		goto done;

	if (client_id->service != data->type)
		goto done;

	service = g_try_new0(struct qmi_service, 1);
	if (!service)
		goto done;

	service->ref_count = 1;
	service->device = data->device;
	service->shared = data->shared;

	service->type = data->type;
	service->major = data->major;
	service->minor = data->minor;

	service->client_id = client_id->client;

	__debug_device(device, "service created [client=%d,type=%d]",
					service->client_id, service->type);

	hash_id = service->type | (service->client_id << 8);

	g_hash_table_replace(device->service_list,
				GUINT_TO_POINTER(hash_id), service);

done:
	data->func(service, data->user_data);

	qmi_service_unref(service);

	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);
}

static void service_create_discover(uint8_t count,
			const struct qmi_version *list, void *user_data)
{
	struct service_create_data *data = user_data;
	struct qmi_device *device = data->device;
	struct qmi_request *req;
	struct qmi_control_hdr *hdr;
	unsigned char client_req[] = { 0x01, 0x01, 0x00, data->type };
	unsigned int i;

	__debug_device(device, "service create [type=%d]", data->type);

	for (i = 0; i < count; i++) {
		if (list[i].type == data->type) {
			data->major = list[i].major;
			data->minor = list[i].minor;
			break;
		}
	}

	req = __request_alloc(QMI_SERVICE_CONTROL, 0x00,
			QMI_CTL_GET_CLIENT_ID, QMI_CONTROL_HDR_SIZE,
			client_req, sizeof(client_req),
			service_create_callback, data, (void **) &hdr);
	if (!req) {
		if (data->timeout > 0)
			g_source_remove(data->timeout);

		g_timeout_add_seconds(0, service_create_reply, data);
		return;
	}

	if (device->next_control_tid < 1)
		device->next_control_tid = 1;

	hdr->type = 0x00;
	hdr->transaction = device->next_control_tid++;

	__request_submit(device, req, hdr->transaction);
}

static bool service_create(struct qmi_device *device, bool shared,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct service_create_data *data;

	data = g_try_new0(struct service_create_data, 1);
	if (!data)
		return false;

	data->device = device;
	data->shared = shared;
	data->type = type;
	data->func = func;
	data->user_data = user_data;
	data->destroy = destroy;

	if (device->version_list) {
		service_create_discover(device->version_count,
						device->version_list, data);
		goto done;
	}

	if (qmi_device_discover(device, service_create_discover, data, NULL))
		goto done;

	g_free(data);

	return false;

done:
	data->timeout = g_timeout_add_seconds(8, service_create_reply, data);

	return true;
}

bool qmi_service_create(struct qmi_device *device,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	if (!device || !func)
		return false;

	if (type == QMI_SERVICE_CONTROL)
		return false;

	return service_create(device, false, type, func, user_data, destroy);
}

struct service_create_shared_data {
	struct qmi_service *service;
	qmi_create_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
};

static gboolean service_create_shared_reply(gpointer user_data)
{
	struct service_create_shared_data *data = user_data;

	data->func(data->service, data->user_data);

	qmi_service_unref(data->service);

	if (data->destroy)
		data->destroy(data->user_data);

	g_free(data);

	return FALSE;
}

bool qmi_service_create_shared(struct qmi_device *device,
				uint8_t type, qmi_create_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct qmi_service *service;
	unsigned int type_val = type;

	if (!device || !func)
		return false;

	if (type == QMI_SERVICE_CONTROL)
		return false;

	service = g_hash_table_find(device->service_list,
			__service_compare_shared, GUINT_TO_POINTER(type_val));
	if (service) {
		struct service_create_shared_data *data;

		data = g_try_new0(struct service_create_shared_data, 1);
		if (!data)
			return false;

		data->service = qmi_service_ref(service);

		data->func = func;
		data->user_data = user_data;
		data->destroy = destroy;

		g_timeout_add(0, service_create_shared_reply, data);

		return 0;
	}

	return service_create(device, true, type, func, user_data, destroy);
}

static void service_release_callback(uint16_t message, uint16_t length,
					const void *buffer, void *user_data)
{
	struct qmi_service *service = user_data;

	if (service->device)
		service->device->release_users--;

	g_free(service);
}

struct qmi_service *qmi_service_ref(struct qmi_service *service)
{
	if (!service)
		return NULL;

	__sync_fetch_and_add(&service->ref_count, 1);

	return service;
}

void qmi_service_unref(struct qmi_service *service)
{
	unsigned int hash_id;

	if (!service)
                return;

	if (__sync_sub_and_fetch(&service->ref_count, 1))
		return;

	if (!service->device) {
		g_free(service);
		return;
	}

	qmi_service_cancel_all(service);
	qmi_service_unregister_all(service);

	hash_id = service->type | (service->client_id << 8);

	g_hash_table_steal(service->device->service_list,
					GUINT_TO_POINTER(hash_id));

	service->device->release_users++;

	release_client(service->device, service->type, service->client_id,
					service_release_callback, service);
}

const char *qmi_service_get_identifier(struct qmi_service *service)
{
	if (!service)
		return NULL;

	return __service_type_to_string(service->type);
}

bool qmi_service_get_version(struct qmi_service *service,
					uint16_t *major, uint16_t *minor)
{
	if (!service)
		return false;

	if (major)
		*major = service->major;

	if (minor)
		*minor = service->minor;

	return true;
}

struct service_send_data {
	struct qmi_service *service;
	struct qmi_param *param;
	qmi_result_func_t func;
	void *user_data;
	qmi_destroy_func_t destroy;
};

static void service_send_free(struct service_send_data *data)
{
	if (data->destroy)
		data->destroy(data->user_data);

	qmi_param_free(data->param);

	g_free(data);
}

static void service_send_callback(uint16_t message, uint16_t length,
					const void *buffer, void *user_data)
{
	struct service_send_data *data = user_data;
	const struct qmi_result_code *result_code;
	uint16_t len;
	struct qmi_result result;

	result.message = message;
	result.data = buffer;
	result.length = length;

	result_code = tlv_get(buffer, length, 0x02, &len);
	if (!result_code)
		goto done;

	if (len != QMI_RESULT_CODE_SIZE)
		goto done;

	result.result = GUINT16_FROM_LE(result_code->result);
	result.error = GUINT16_FROM_LE(result_code->error);

done:
	if (data->func)
		data->func(&result, data->user_data);

	service_send_free(data);
}

uint16_t qmi_service_send(struct qmi_service *service,
				uint16_t message, struct qmi_param *param,
				qmi_result_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct qmi_device *device;
	struct service_send_data *data;
	struct qmi_request *req;
	struct qmi_service_hdr *hdr;

	if (!service)
		return 0;

	if (!service->client_id)
		return 0;

	device = service->device;
	if (!device)
		return 0;

	data = g_try_new0(struct service_send_data, 1);
	if (!data)
		return 0;

	data->service = service;
	data->param = param;
	data->func = func;
	data->user_data = user_data;
	data->destroy = destroy;

	req = __request_alloc(service->type, service->client_id,
				message, QMI_SERVICE_HDR_SIZE,
				data->param ? data->param->data : NULL,
				data->param ? data->param->length : 0,
				service_send_callback, data, (void **) &hdr);
	if (!req) {
		g_free(data);
		return 0;
	}

	if (device->next_service_tid < 256)
		device->next_service_tid = 256;

	hdr->type = 0x00;
	hdr->transaction = device->next_service_tid++;

	__request_submit(device, req, hdr->transaction);

	return hdr->transaction;
}

bool qmi_service_cancel(struct qmi_service *service, uint16_t id)
{
	unsigned int tid = id;
	struct qmi_device *device;
	struct qmi_request *req;
	GList *list;

	if (!service || !tid)
		return false;

	if (!service->client_id)
		return false;

	device = service->device;
	if (!device)
		return false;

	list = g_queue_find_custom(device->req_queue,
				GUINT_TO_POINTER(tid), __request_compare);
	if (list) {
		req = list->data;

		g_queue_delete_link(device->req_queue, list);
	} else {
		list = g_queue_find_custom(device->service_queue,
				GUINT_TO_POINTER(tid), __request_compare);
		if (!list)
			return false;

		req = list->data;

		g_queue_delete_link(device->service_queue, list);
	}

	service_send_free(req->user_data);

	__request_free(req, NULL);

	return true;
}

static GQueue *remove_client(GQueue *queue, uint8_t client)
{
	GQueue *new_queue;
	GList *list;

	new_queue = g_queue_new();

	while (1) {
		struct qmi_request *req;

		list = g_queue_pop_head_link(queue);
		if (!list)
			break;

		req = list->data;

		if (!req->client || req->client != client) {
			g_queue_push_tail_link(new_queue, list);
			continue;
		}

		service_send_free(req->user_data);

		__request_free(req, NULL);
	}

	g_queue_free(queue);

	return new_queue;
}

bool qmi_service_cancel_all(struct qmi_service *service)
{
	struct qmi_device *device;

	if (!service)
		return false;

	if (!service->client_id)
		return false;

	device = service->device;
	if (!device)
		return false;

	device->req_queue = remove_client(device->req_queue,
						service->client_id);

	device->service_queue = remove_client(device->service_queue,
							service->client_id);

	return true;
}

uint16_t qmi_service_register(struct qmi_service *service,
				uint16_t message, qmi_result_func_t func,
				void *user_data, qmi_destroy_func_t destroy)
{
	struct qmi_notify *notify;

	if (!service || !func)
		return 0;

	notify = g_try_new0(struct qmi_notify, 1);
	if (!notify)
		return 0;

	if (service->next_notify_id < 1)
		service->next_notify_id = 1;

	notify->id = service->next_notify_id++;
	notify->message = message;
	notify->callback = func;
	notify->user_data = user_data;
	notify->destroy = destroy;

	service->notify_list = g_list_append(service->notify_list, notify);

	return notify->id;
}

bool qmi_service_unregister(struct qmi_service *service, uint16_t id)
{
	unsigned int nid = id;
	struct qmi_notify *notify;
	GList *list;

	if (!service || !id)
		return false;

	list = g_list_find_custom(service->notify_list,
				GUINT_TO_POINTER(nid), __notify_compare);
	if (!list)
		return false;

	notify = list->data;

	service->notify_list = g_list_delete_link(service->notify_list, list);

	__notify_free(notify, NULL);

	return true;
}

bool qmi_service_unregister_all(struct qmi_service *service)
{
	if (!service)
		return false;

	g_list_foreach(service->notify_list, __notify_free, NULL);
	g_list_free(service->notify_list);

	service->notify_list = NULL;

	return true;
}
