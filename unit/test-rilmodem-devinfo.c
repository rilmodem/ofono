/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2016 Canonical Ltd.
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
#include <ofono/devinfo.h>
#include <gril.h>

#include "common.h"
#include "ril_constants.h"
#include "rilmodem-test-server.h"

enum devinfo_driver_call {
	DEVINFO_QUERY_MANUFACTURER,
	DEVINFO_QUERY_MODEL,
	DEVINFO_QUERY_REVISION,
	DEVINFO_QUERY_SERIAL,
};

struct devinfo_data {
	struct rilmodem_test_data rtd;
	enum devinfo_driver_call call_type;

	/* Output parameters */
	enum ofono_error_type error_type;
	const char *attribute;
};

struct rilmodem_devinfo_data {
	GRil *ril;
	struct ofono_modem *modem;
	const struct devinfo_data *test_data;
	struct ofono_devinfo *devinfo;
	struct server_data *serverd;
};

static const struct ofono_devinfo_driver *devinfo_drv;

/* Declarations && Re-implementations of core functions. */
void ril_devinfo_exit(void);
void ril_devinfo_init(void);

struct ofono_devinfo {
	void *driver_data;
	const struct rilmodem_devinfo_data *rdid;
};

static GMainLoop *mainloop;

int ofono_devinfo_driver_register(const struct ofono_devinfo_driver *d)
{
	if (devinfo_drv == NULL)
		devinfo_drv = d;

	return 0;
}

void ofono_devinfo_driver_unregister(const struct ofono_devinfo_driver *d)
{
	devinfo_drv = NULL;
}

struct ofono_devinfo *ofono_devinfo_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data)
{
	struct rilmodem_devinfo_data *rdid = data;
	struct ofono_devinfo *devinfo = g_malloc0(sizeof(*devinfo));
	int retval;

	devinfo->rdid = rdid;

	retval = devinfo_drv->probe(devinfo, OFONO_RIL_VENDOR_AOSP, rdid->ril);
	g_assert(retval == 0);

	return devinfo;
}

void ofono_devinfo_register(struct ofono_devinfo *devinfo)
{
}

void ofono_devinfo_set_data(struct ofono_devinfo *devinfo, void *data)
{
	devinfo->driver_data = data;
}

void *ofono_devinfo_get_data(struct ofono_devinfo *devinfo)
{
	return devinfo->driver_data;
}

/*
 * As all our architectures are little-endian except for
 * PowerPC, and the Binder wire-format differs slightly
 * depending on endian-ness, the following guards against test
 * failures when run on PowerPC.
 */
#if BYTE_ORDER == LITTLE_ENDIAN

/*
 * When querying manufacturer or model, there is no actual data interchanged
 * through the socket, as fixed strings are returned currently. We set
 * unsol_test to true so the server does not wait for data.
 */

/* Test query_manufacturer driver call */
static const struct devinfo_data testdata_devinfo_manufacturer_valid_1 = {
	.rtd = { .unsol_test = TRUE },
	.call_type = DEVINFO_QUERY_MANUFACTURER,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.attribute = "Fake Manufacturer",
};

/* Test query_model driver call */
static const struct devinfo_data testdata_devinfo_model_valid_1 = {
	.rtd = { .unsol_test = TRUE },
	.call_type = DEVINFO_QUERY_MODEL,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.attribute = "Fake Modem Model",
};

/* Test RIL_REQUEST_BASEBAND_VERSION */
static const guchar req_baseband_version_parcel_valid_1[] = {
	0x00, 0x00, 0x00, 0x08, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const guchar rsp_baseband_version_data_valid_1[] = {
	0x1f, 0x00, 0x00, 0x00, 0x53, 0x00, 0x33, 0x00, 0x33, 0x00, 0x33, 0x00,
	0x5f, 0x00, 0x4d, 0x00, 0x38, 0x00, 0x36, 0x00, 0x5f, 0x00, 0x32, 0x00,
	0x30, 0x00, 0x31, 0x00, 0x35, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00,
	0x32, 0x00, 0x5f, 0x00, 0x43, 0x00, 0x48, 0x00, 0x4e, 0x00, 0x4f, 0x00,
	0x50, 0x00, 0x45, 0x00, 0x4e, 0x00, 0x5f, 0x00, 0x4d, 0x00, 0x5a, 0x00,
	0x30, 0x00, 0x32, 0x00, 0x34, 0x00, 0x00, 0x00
};

static const struct devinfo_data testdata_devinfo_baseband_version_valid_1 = {
	.rtd = {
		.req_data = req_baseband_version_parcel_valid_1,
		.req_size = sizeof(req_baseband_version_parcel_valid_1),
		.rsp_data = rsp_baseband_version_data_valid_1,
		.rsp_size = sizeof(rsp_baseband_version_data_valid_1),
		.rsp_error = RIL_E_SUCCESS,
	},
	.call_type = DEVINFO_QUERY_REVISION,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.attribute = "S333_M86_20151002_CHNOPEN_MZ024",
};

/* Test RIL_REQUEST_BASEBAND_VERSION */
static const guchar req_baseband_version_parcel_invalid_1[] = {
	0x00, 0x00, 0x00, 0x08, 0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct devinfo_data testdata_devinfo_baseband_version_invalid_1 = {
	.rtd = {
		.req_data = req_baseband_version_parcel_invalid_1,
		.req_size = sizeof(req_baseband_version_parcel_invalid_1),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_RADIO_NOT_AVAILABLE,
	},
	.call_type = DEVINFO_QUERY_REVISION,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
	.attribute = NULL,
};

/* Test RIL_REQUEST_GET_IMEI */
static const guchar req_get_imei_parcel_valid_1[] = {
	0x00, 0x00, 0x00, 0x08, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const guchar rsp_get_imei_data_valid_1[] = {
	0x0f, 0x00, 0x00, 0x00, 0x38, 0x00, 0x36, 0x00, 0x37, 0x00, 0x31, 0x00,
	0x30, 0x00, 0x35, 0x00, 0x30, 0x00, 0x32, 0x00, 0x32, 0x00, 0x33, 0x00,
	0x37, 0x00, 0x34, 0x00, 0x30, 0x00, 0x34, 0x00, 0x36, 0x00, 0x00, 0x00
};

static const struct devinfo_data testdata_devinfo_get_imei_valid_1 = {
	.rtd = {
		.req_data = req_get_imei_parcel_valid_1,
		.req_size = sizeof(req_get_imei_parcel_valid_1),
		.rsp_data = rsp_get_imei_data_valid_1,
		.rsp_size = sizeof(rsp_get_imei_data_valid_1),
		.rsp_error = RIL_E_SUCCESS,
	},
	.call_type = DEVINFO_QUERY_SERIAL,
	.error_type = OFONO_ERROR_TYPE_NO_ERROR,
	.attribute = "867105022374046",
};

/* Test RIL_REQUEST_GET_IMEI */
static const guchar req_get_imei_parcel_invalid_1[] = {
	0x00, 0x00, 0x00, 0x08, 0x26, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const struct devinfo_data testdata_devinfo_get_imei_invalid_1 = {
	.rtd = {
		.req_data = req_get_imei_parcel_invalid_1,
		.req_size = sizeof(req_get_imei_parcel_invalid_1),
		.rsp_data = NULL,
		.rsp_size = 0,
		.rsp_error = RIL_E_RADIO_NOT_AVAILABLE,
	},
	.call_type = DEVINFO_QUERY_SERIAL,
	.error_type = OFONO_ERROR_TYPE_FAILURE,
	.attribute = NULL,
};

static void devinfo_callback(const struct ofono_error *error,
					const char *attribute, void *data)
{
	struct rilmodem_devinfo_data *rdid = data;

	g_assert(error->type == rdid->test_data->error_type);
	g_assert_cmpstr(attribute, ==, rdid->test_data->attribute);

	g_main_loop_quit(mainloop);
}

static void server_connect_cb(gpointer data)
{
	struct rilmodem_devinfo_data *rdid = data;

	/* This causes local impl of _create() to call driver's probe func. */
	rdid->devinfo = ofono_devinfo_create(NULL, OFONO_RIL_VENDOR_AOSP,
							"rilmodem", rdid);

	switch (rdid->test_data->call_type) {
	case DEVINFO_QUERY_MANUFACTURER:
		devinfo_drv->query_manufacturer(rdid->devinfo,
							devinfo_callback, rdid);
		break;
	case DEVINFO_QUERY_MODEL:
		devinfo_drv->query_model(rdid->devinfo, devinfo_callback, rdid);
		break;
	case DEVINFO_QUERY_REVISION:
		devinfo_drv->query_revision(rdid->devinfo,
							devinfo_callback, rdid);
		break;
	case DEVINFO_QUERY_SERIAL:
		devinfo_drv->query_serial(rdid->devinfo,
							devinfo_callback, rdid);
		break;
	};
}

/*
 * This unit test:
 *  - does some test data setup
 *  - configures a dummy server socket
 *  - creates a new gril client instance
 *    - triggers a connect to the dummy
 *      server socket
 *  - starts a mainloop
 */
static void test_devinfo_socket(gconstpointer data)
{
	const struct devinfo_data *devinfod = data;
	struct rilmodem_devinfo_data *rdid;

	ril_devinfo_init();

	rdid = g_malloc0(sizeof(*rdid));

	rdid->test_data = devinfod;

	rdid->serverd = rilmodem_test_server_create(&server_connect_cb,
							&devinfod->rtd, rdid);

	rdid->ril = g_ril_new(rilmodem_test_get_socket_name(rdid->serverd),
							OFONO_RIL_VENDOR_AOSP);
	g_assert(rdid->ril != NULL);

	mainloop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(mainloop);
	g_main_loop_unref(mainloop);

	devinfo_drv->remove(rdid->devinfo);
	g_free(rdid->devinfo);
	g_ril_unref(rdid->ril);
	g_free(rdid);

	rilmodem_test_server_close(rdid->serverd);

	ril_devinfo_exit();
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
	g_test_add_data_func("/test-rilmodem-devinfo/manufacturer/valid/1",
				&testdata_devinfo_manufacturer_valid_1,
				test_devinfo_socket);
	g_test_add_data_func("/test-rilmodem-devinfo/model/valid/1",
				&testdata_devinfo_model_valid_1,
				test_devinfo_socket);
	g_test_add_data_func("/test-rilmodem-devinfo/revision/valid/1",
				&testdata_devinfo_baseband_version_valid_1,
				test_devinfo_socket);
	g_test_add_data_func("/test-rilmodem-devinfo/revision/invalid/1",
				&testdata_devinfo_baseband_version_invalid_1,
				test_devinfo_socket);
	g_test_add_data_func("/test-rilmodem-devinfo/serial/valid/1",
				&testdata_devinfo_get_imei_valid_1,
				test_devinfo_socket);
	g_test_add_data_func("/test-rilmodem-devinfo/serial/invalid/1",
				&testdata_devinfo_get_imei_invalid_1,
				test_devinfo_socket);
#endif
	return g_test_run();
}
