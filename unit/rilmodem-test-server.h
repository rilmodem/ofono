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

struct server_data;

struct rilmodem_test_data {
	const unsigned char *req_data;

	const size_t req_size;

	uint32_t rsp_error;
	const unsigned char *rsp_data;
	const size_t rsp_size;
	gboolean unsol_test;
};

typedef void (*ConnectFunc)(void *data);

void rilmodem_test_server_close(struct server_data *sd);

struct server_data *rilmodem_test_server_create(ConnectFunc connect,
				const struct rilmodem_test_data *test_data,
				void *data);

void rilmodem_test_server_write(struct server_data *sd,
						const unsigned char *buf,
						const size_t buf_len);

const char *rilmodem_test_get_socket_name(struct server_data *sd);
