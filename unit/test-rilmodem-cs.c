/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2015 Canonical Ltd.
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
#include <assert.h>
#include <errno.h>
#include <glib.h>
#include <stdio.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <ofono/modem.h>
#include <ofono/types.h>
#include <ofono/call-settings.h>
#include <gril.h>

#include "common.h"
#include "ril_constants.h"

#define MAX_REQUEST_SIZE 4096

static GMainLoop *mainloop;

static const struct ofono_call_settings_driver *csdriver;

struct rilmodemcs_data {
	GRil *ril;
	int sk;
	gint server_watch;
	GIOChannel *server_io;
	struct ofono_modem *modem;
	gconstpointer test_data;
	struct ofono_call_settings *cs;
};

/* Warning: length is stored in network order */
struct rsp_hdr {
	uint32_t length;
	uint32_t unsolicited;
	uint32_t serial;
	uint32_t error;
};

typedef gboolean (*StartFunc)(gpointer data);

struct cs_data {
	StartFunc start_func;
	gint param_int1;
	gint param_int2;

	const guchar *parcel_data;

	const gsize parcel_size;

	uint32_t rsp_error;
	const guchar *rsp_data;
	const gsize rsp_size;
	enum ofono_error_type error_type;
	gint cb_int1;
	gint cb_int2;
};

static void status_query_callback(const struct ofono_error *error, int status,
								 gpointer data)
{
	struct rilmodemcs_data *rcsd = data;
	const struct cs_data *csd = rcsd->test_data;

	g_assert(error->type == csd->error_type);

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		g_assert(status == csd->cb_int1);

	g_main_loop_quit(mainloop);
}

static void clir_query_callback(const struct ofono_error *error, int override,
						int network, gpointer data)
{
	struct rilmodemcs_data *rcsd = data;
	const struct cs_data *csd = rcsd->test_data;

	g_assert(error->type == csd->error_type);

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		g_assert(override == csd->cb_int1);
		g_assert(network == csd->cb_int2);
	}

	g_main_loop_quit(mainloop);
}

static void set_callback(const struct ofono_error *error, gpointer data)
{
	struct rilmodemcs_data *rcsd = data;
	const struct cs_data *csd = rcsd->test_data;

	g_assert(error->type == csd->error_type);

	g_main_loop_quit(mainloop);
}

static gboolean trigger_clip_query(gpointer data)
{
	struct rilmodemcs_data *rcsd = data;

	g_assert(csdriver->clip_query != NULL);
	csdriver->clip_query(rcsd->cs, status_query_callback, rcsd);

	return FALSE;
}

static gboolean trigger_cw_query(gpointer data)
{
	struct rilmodemcs_data *rcsd = data;

	g_assert(csdriver->cw_query != NULL);

	/* cls is explicitly ignored by rilmodem; just use 0 */
	csdriver->cw_query(rcsd->cs, 0, status_query_callback, rcsd);

	return FALSE;
}

static gboolean trigger_cw_set(gpointer data)
{
	struct rilmodemcs_data *rcsd = data;
	const struct cs_data *csd = rcsd->test_data;

	g_assert(csdriver->cw_set != NULL);

	csdriver->cw_set(rcsd->cs, csd->param_int1, csd->param_int2,
						set_callback, rcsd);

	return FALSE;
}

static gboolean trigger_clir_query(gpointer data)
{
	struct rilmodemcs_data *rcsd = data;

	g_assert(csdriver->clir_query != NULL);
	csdriver->clir_query(rcsd->cs, clir_query_callback, rcsd);

	return FALSE;
}

static gboolean trigger_clir_set(gpointer data)
{
	struct rilmodemcs_data *rcsd = data;
	const struct cs_data *csd = rcsd->test_data;

	g_assert(csdriver->clir_set != NULL);
	csdriver->clir_set(rcsd->cs, csd->param_int1, set_callback, rcsd);

	return FALSE;
}

/* RIL_REQUEST_QUERY_CLIP */
static const guchar req_clip_query_parcel_1[] = {
	0x00, 0x00, 0x00, 0x08, 0x37, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* reply data for QUErY_CLIP: 0x01 = 'CLIP provisioned' */
static const guchar rsp_clip_query_data_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct cs_data testdata_clip_query_valid_1 = {
	.start_func = trigger_clip_query,
	.parcel_data = req_clip_query_parcel_1,
	.parcel_size = sizeof(req_clip_query_parcel_1),
	.rsp_data = rsp_clip_query_data_1,
	.rsp_size = sizeof(rsp_clip_query_data_1),
	.rsp_error = RIL_E_SUCCESS,
	.cb_int1 = 1,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* reply data for QUErY_CLIP: invalid num_params=0x02' */
static const guchar rsp_clip_query_data_2[] = {
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

/* reply parse error causes status to be returned as -1 */
static const struct cs_data testdata_clip_query_invalid_1 = {
	.start_func = trigger_clip_query,
	.parcel_data = req_clip_query_parcel_1,
	.parcel_size = sizeof(req_clip_query_parcel_1),
	.rsp_data = rsp_clip_query_data_2,
	.rsp_size = sizeof(rsp_clip_query_data_2),
	.cb_int1 = -1,
	.rsp_error = RIL_E_SUCCESS,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* error triggered by RIL reply error */
static const struct cs_data testdata_clip_query_invalid_2 = {
	.start_func = trigger_clip_query,
	.parcel_data = req_clip_query_parcel_1,
	.parcel_size = sizeof(req_clip_query_parcel_1),
	.rsp_error = RIL_E_GENERIC_FAILURE,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/* RIL_REQUEST_QUERY_CALL_WAITING */
static const guchar req_cw_query_parcel_1[] = {
	0x00, 0x00, 0x00, 0x10, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* reply data for QUErY_CALL_WAITING: 1='enabled' 3='data|voice' */
static const guchar rsp_cw_query_data_1[] = {
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3, 0x00, 0x00, 0x00
};

static const struct cs_data testdata_cw_query_valid_1 = {
	.start_func = trigger_cw_query,
	.parcel_data = req_cw_query_parcel_1,
	.parcel_size = sizeof(req_cw_query_parcel_1),
	.rsp_data = rsp_cw_query_data_1,
	.rsp_size = sizeof(rsp_cw_query_data_1),
	.rsp_error = RIL_E_SUCCESS,
	.cb_int1 = 3,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* reply data for QUErY_CALL_WAITING: invalid num_params=0x00' */
static const guchar rsp_cw_query_data_2[] = {
	0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

/* reply parse error causes status to be returned as -1 */
static const struct cs_data testdata_cw_query_invalid_1 = {
	.start_func = trigger_cw_query,
	.parcel_data = req_cw_query_parcel_1,
	.parcel_size = sizeof(req_cw_query_parcel_1),
	.rsp_data = rsp_cw_query_data_2,
	.rsp_size = sizeof(rsp_cw_query_data_2),
	.cb_int1 = -1,
	.rsp_error = RIL_E_SUCCESS,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* GENERIC_FAILURE returned in RIL reply */
static const struct cs_data testdata_cw_query_invalid_2 = {
	.start_func = trigger_cw_query,
	.parcel_data = req_cw_query_parcel_1,
	.parcel_size = sizeof(req_cw_query_parcel_1),
	.rsp_data = rsp_cw_query_data_2,
	.rsp_size = sizeof(rsp_cw_query_data_2),
	.cb_int1 = -1,
	.rsp_error = RIL_E_GENERIC_FAILURE,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/* RIL_REQUEST_SET_CALL_WAITING: enabled cls=BEARER_CLASS_DEFAULT (7) */
/* Note - driver atom checks for cls=7, and changes to cls=1 */
static const guchar req_cw_set_enabled_parcel_1[] = {
	0x00, 0x00, 0x00, 0x14, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

static const struct cs_data testdata_cw_set_valid_1 = {
	.start_func = trigger_cw_set,
	.param_int1 = 1,
	.param_int2 = BEARER_CLASS_DEFAULT,
	.parcel_data = req_cw_set_enabled_parcel_1,
	.parcel_size = sizeof(req_cw_set_enabled_parcel_1),
	.rsp_error = RIL_E_SUCCESS,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* RIL_REQUEST_SET_CALL_WAITING: disabled cls=0 */
static const guchar req_cw_set_disabled_parcel_2[] = {
	0x00, 0x00, 0x00, 0x14, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* GENERIC_FAILURE returned in RIL reply */
static const struct cs_data testdata_cw_set_invalid_1 = {
	.start_func = trigger_cw_set,
	.param_int1 = 0,
	.param_int2 = 0,
	.parcel_data = req_cw_set_disabled_parcel_2,
	.parcel_size = sizeof(req_cw_set_disabled_parcel_2),
	.rsp_error = RIL_E_GENERIC_FAILURE,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/* RIL_REQUEST_GET_CLIR */
static const guchar req_clir_query_parcel_1[] = {
	0x00, 0x00, 0x00, 0x08, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/* reply data for GET_CLIR: m=2 n=4; see TS 27.007 Section 7.7 */
static const guchar rsp_clir_query_data_1[] = {
	0x02, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00
};

static const struct cs_data testdata_clir_query_valid_1 = {
	.start_func = trigger_clir_query,
	.parcel_data = req_clir_query_parcel_1,
	.parcel_size = sizeof(req_clir_query_parcel_1),
	.rsp_data = rsp_clir_query_data_1,
	.rsp_size = sizeof(rsp_clir_query_data_1),
	.cb_int1 = 2,
	.cb_int2 = 4,
	.rsp_error = RIL_E_SUCCESS,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* invalid reply data for GET_CLIR: num params is 3 instead of 2 */
static const guchar rsp_clir_query_data_2[] = {
	0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00
};
static const struct cs_data testdata_clir_query_invalid_1 = {
	.start_func = trigger_clir_query,
	.parcel_data = req_clir_query_parcel_1,
	.parcel_size = sizeof(req_clir_query_parcel_1),
	.rsp_data = rsp_clir_query_data_2,
	.rsp_size = sizeof(rsp_clir_query_data_2),
	.rsp_error = RIL_E_SUCCESS,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/* RIL_REQUEST_SET_CLIR: mode=DEFAULT */
static const guchar req_clir_set_mode0_parcel_1[] = {
	0x00, 0x00, 0x00, 0x10, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct cs_data testdata_clir_set_valid_1 = {
	.start_func = trigger_clir_set,
	.param_int1 = OFONO_CLIR_OPTION_DEFAULT,
	.parcel_data = req_clir_set_mode0_parcel_1,
	.parcel_size = sizeof(req_clir_set_mode0_parcel_1),
	.rsp_error = RIL_E_SUCCESS,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
};

/* RIL_REQUEST_SET_CLIR: mode=INVOCATION */
static const guchar req_clir_set_mode0_parcel_2[] = {
	0x00, 0x00, 0x00, 0x10, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00
};

/* GENERIC_FAILURE returned in RIL reply */
static const struct cs_data testdata_clir_set_invalid_1 = {
	.start_func = trigger_clir_set,
	.param_int1 = OFONO_CLIR_OPTION_INVOCATION,
	.parcel_data = req_clir_set_mode0_parcel_2,
	.parcel_size = sizeof(req_clir_set_mode0_parcel_2),
	.rsp_error = RIL_E_GENERIC_FAILURE,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
};

/* Declarations && Re-implementations of core functions. */
void ril_call_settings_exit(void);
void ril_call_settings_init(void);

struct ofono_call_settings {
	void *driver_data;
};

struct ofono_call_settings *ofono_call_settings_create(struct ofono_modem *modem,
							unsigned int vendor,
							const char *driver,
							void *data)
{
	struct rilmodemcs_data *rcsd = data;
	struct ofono_call_settings *cs = g_new0(struct ofono_call_settings, 1);
	int retval;

	retval = csdriver->probe(cs, OFONO_RIL_VENDOR_AOSP, rcsd->ril);
	g_assert(retval == 0);

	return cs;
}

int ofono_call_settings_driver_register(const struct ofono_call_settings_driver *d)
{
	if (csdriver == NULL)
		csdriver = d;

	return 0;
}

void ofono_call_settings_set_data(struct ofono_call_settings *cs, void *data)
{
	cs->driver_data = data;
}

void *ofono_call_settings_get_data(struct ofono_call_settings *cs)
{
	return cs->driver_data;
}

void ofono_call_settings_register(struct ofono_call_settings *cs)
{
	;
}

void ofono_call_settings_driver_unregister(const struct ofono_call_settings_driver *d)
{
	;
}

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

static gboolean read_server(gpointer data)
{
	GIOStatus status;
	struct rilmodemcs_data *rcsd = data;
	gsize offset, rbytes, wbytes;
	gchar *buf, *bufp;
	uint32_t req_serial;
	struct rsp_hdr rsp;

	/*
	 * FIXME: separate out verification from here, so read_server doesn't
	 * need to know about cs_data.
	 */
	const struct cs_data *csd = rcsd->test_data;

	buf = g_malloc0(MAX_REQUEST_SIZE);

	status = g_io_channel_read_chars(rcsd->server_io, buf, MAX_REQUEST_SIZE,
								&rbytes, NULL);
	g_assert(status == G_IO_STATUS_NORMAL);
	g_assert(rbytes == csd->parcel_size);

	/* validate len, and request_id */
	g_assert(!memcmp(buf, csd->parcel_data, (sizeof(uint32_t) * 2)));

	/*
	 * header: size (uint32), reqid (uin32), serial (uint32)
	 * header size == 16 ( excludes sizeof(size) )
	 */

	/* advance past request_no */
	bufp = buf + (sizeof(uint32_t) * 2);

	req_serial = (uint32_t) *bufp;

	/* advance past serial_no */
	bufp += sizeof(uint32_t);

	/* validate the rest of the parcel... */
	offset = (sizeof(uint32_t) * 3);
	g_assert(!memcmp(bufp, csd->parcel_data + offset,
						csd->parcel_size - offset));

	/* Length does not include the length field. Network order. */
	rsp.length = htonl(sizeof(rsp) - sizeof(rsp.length) + csd->rsp_size);
	rsp.unsolicited = 0;
	rsp.serial = req_serial;
	rsp.error = csd->rsp_error;

	/* copy header */
	memcpy(buf, &rsp, sizeof(rsp));

	if (csd->rsp_size) {
		bufp = buf + sizeof(rsp);

		memcpy(bufp, csd->rsp_data, csd->rsp_size);
	}


	status = g_io_channel_write_chars(rcsd->server_io,
					buf,
					sizeof(rsp) + csd->rsp_size,
					&wbytes, NULL);

	/* FIXME: assert wbytes is correct */

	g_assert(status == G_IO_STATUS_NORMAL);

	g_free(buf);
	g_io_channel_unref(rcsd->server_io);

	return FALSE;
}

static gboolean on_socket_connected(GIOChannel *chan, GIOCondition cond,
								gpointer data)
{
	struct rilmodemcs_data *rcsd = data;
	const struct cs_data *csd = rcsd->test_data;
	struct sockaddr saddr;
	unsigned int len = sizeof(saddr);
	int fd;
	GIOChannel *server_io = NULL;
	GIOStatus status;

	g_assert(cond == G_IO_IN);

	fd = accept(rcsd->sk, &saddr, &len);
	g_assert(fd != -1);

	server_io = g_io_channel_unix_new(fd);
	g_assert(server_io != NULL);

	/* This causes local impl of _create() to call driver's probe func. */
	rcsd->cs = ofono_call_settings_create(NULL, OFONO_RIL_VENDOR_AOSP,
							"rilmodem", rcsd);

	/* add_idle doesn't work, read blocks main loop!!! */
	g_assert(csd->start_func(rcsd) == FALSE);

	status = g_io_channel_set_encoding(server_io, NULL, NULL);
	g_assert(status == G_IO_STATUS_NORMAL);

	g_io_channel_set_buffered(server_io, FALSE);
	g_io_channel_set_close_on_unref(server_io, TRUE);

	rcsd->server_io = server_io;

	g_idle_add(read_server, rcsd);

	/* single-shot callback */
	return FALSE;
}

static void create_server_socket(const char *sock_path,
					struct rilmodemcs_data *rcsd)
{
	GIOChannel *io;
	struct sockaddr_un addr;
	int retval;

	rcsd->sk = socket(AF_UNIX, SOCK_STREAM, 0);
	g_assert(rcsd->sk);

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

	/* Unlink any existing socket for this session */
	unlink(addr.sun_path);

	retval = bind(rcsd->sk, (struct sockaddr *) &addr, sizeof(addr));
	g_assert(retval >= 0);

	retval = listen(rcsd->sk, 0);
	g_assert(retval >= 0);

	io = g_io_channel_unix_new(rcsd->sk);
	g_assert(io != NULL);

	g_io_channel_set_close_on_unref(io, TRUE);
	g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK, NULL);

	rcsd->server_watch = g_io_add_watch_full(io,
				G_PRIORITY_DEFAULT,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				on_socket_connected, rcsd, NULL);

	g_io_channel_unref(io);
}

/*
 * This unit test:
 *  - does some test data setup
 *  - configures a dummy server socket
 *    - on_socket_connected: callback for
 *      incoming socket connects
 *  - creates a new gril client instance
 *    - triggers a connect to the dummy
 *      server socket
 *  - starts a mainloop
 */
static void test_cs_func(gconstpointer data)
{
	const struct cs_data *csd = data;
	struct rilmodemcs_data *rcsd;

	ril_call_settings_init();

	rcsd = g_new0(struct rilmodemcs_data, 1);

	rcsd->test_data = csd;

	create_server_socket("/tmp/unittestril", rcsd);

	rcsd->ril = g_ril_new("/tmp/unittestril", OFONO_RIL_VENDOR_AOSP);
	g_assert(rcsd->ril != NULL);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
	g_main_loop_unref(mainloop);

	csdriver->remove(rcsd->cs);
	g_ril_unref(rcsd->ril);
	g_free(rcsd);

	ril_call_settings_exit();
}

#endif

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN
	g_test_add_data_func("/testrilmodemcs/clip_query/valid/1",
					&testdata_clip_query_valid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/clip_query/invalid/1",
					&testdata_clip_query_invalid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/clip_query/invalid/2",
					&testdata_clip_query_invalid_2,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/cw_query/valid/1",
					&testdata_cw_query_valid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/cw_query/invalid/1",
					&testdata_cw_query_invalid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/cw_query/invalid/2",
					&testdata_cw_query_invalid_2,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/cw_set/valid/1",
					&testdata_cw_set_valid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/cw_set/invalid/1",
					&testdata_cw_set_invalid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/clir_query/valid/1",
					&testdata_clir_query_valid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/clir_query/invalid/1",
					&testdata_clir_query_invalid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/clir_set/valid/1",
					&testdata_clir_set_valid_1,
					test_cs_func);

	g_test_add_data_func("/testrilmodemcs/clir_set/invalid/1",
					&testdata_clir_set_invalid_1,
					test_cs_func);

#endif
	return g_test_run();
}
