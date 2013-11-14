/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013 Canonical Ltd.
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

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <ofono.h>
#include <plugin.h>
#include <sim-mnclength.h>

extern struct ofono_plugin_desc __ofono_builtin_mnclength;

struct get_mnclength_test_data {
	const char *imsi;
	int mnc_length;		/* Expected length */
};

/* test data */

static const struct get_mnclength_test_data get_mnclength_0 = {
	.imsi = "214060240111837",
	.mnc_length = -ENOTSUP
};

static const struct get_mnclength_test_data get_mnclength_1 = {
	.imsi = "214060240111837",
	.mnc_length = 2
};

static const struct get_mnclength_test_data get_mnclength_2 = {
	.imsi = "313001740111837",
	.mnc_length = 3
};

static const struct get_mnclength_test_data get_mnclength_3 = {
	.imsi = "352060240111837",
	.mnc_length = 3
};

static const struct get_mnclength_test_data get_mnclength_4 = {
	.imsi = "602060240111837",
	.mnc_length = 2
};

static const struct get_mnclength_test_data get_mnclength_5 = {
	.imsi = "4051000240111837",
	.mnc_length = 2
};

static const struct get_mnclength_test_data get_mnclength_6 = {
	.imsi = "405801240111837",
	.mnc_length = 3
};

static const struct get_mnclength_test_data get_mnclength_7 = {
	.imsi = "714020240111837",
	.mnc_length = 3
};

static const struct get_mnclength_test_data get_mnclength_8 = {
	.imsi = "000000000000000",
	.mnc_length = -ENOENT
};

static const struct get_mnclength_test_data get_mnclength_9 = {
	.imsi = "0xx000000000000",
	.mnc_length = -EINVAL
};

static const struct get_mnclength_test_data get_mnclength_10 = {
	.imsi = "111111000000000",
	.mnc_length = -ENOENT
};

static void test_get_mnclength(gconstpointer data)
{
	int mnc_length;
	const struct get_mnclength_test_data *testdata = data;

	mnc_length = __ofono_sim_mnclength_get_mnclength(testdata->imsi);
	g_assert(mnc_length == testdata->mnc_length);
}

static void test_mnclength_register(void)
{
	g_assert(__ofono_builtin_mnclength.init() == 0);
}

int main(int argc, char **argv)
{
	int res;

	g_test_init(&argc, &argv, NULL);

	g_test_add_data_func("/testmnclength: Test 0",
				&get_mnclength_0,
				test_get_mnclength);

	g_test_add_func("/testmnclength: Test register",
			test_mnclength_register);

	g_test_add_data_func("/testmnclength: Test 1",
				&get_mnclength_1,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 2",
				&get_mnclength_2,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 3",
				&get_mnclength_3,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 4",
				&get_mnclength_4,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 5",
				&get_mnclength_5,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 6",
				&get_mnclength_6,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 7",
				&get_mnclength_7,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 8",
				&get_mnclength_8,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 9",
				&get_mnclength_9,
				test_get_mnclength);
	g_test_add_data_func("/testmnclength: Test 10",
				&get_mnclength_10,
				test_get_mnclength);

	res = g_test_run();

	__ofono_builtin_mnclength.exit();

	return res;
}
