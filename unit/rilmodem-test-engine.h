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

struct engine_data;

enum test_step_type {
	TST_ACTION_SEND,
	TST_ACTION_CALL,
	TST_EVENT_RECEIVE,
	TST_EVENT_CALL,
};

typedef void (*rilmodem_test_engine_cb_t)(void *data);

struct rilmodem_test_step {
	enum test_step_type type;

	union {
		/* For TST_ACTION_CALL */
		rilmodem_test_engine_cb_t call_action;
		/* For TST_ACTION_SEND or TST_EVENT_RECEIVE */
		struct {
			const char *parcel_data;
			const size_t parcel_size;
		};
		/* For TST_EVENT_CALL */
		struct {
			void (*call_func)(void);
			void (*check_func)(void);
		};
	};
};

struct rilmodem_test_data {
	const struct rilmodem_test_step *steps;
	int num_steps;
};

void rilmodem_test_engine_remove(struct engine_data *ed);

struct engine_data *rilmodem_test_engine_create(
				rilmodem_test_engine_cb_t connect,
				const struct rilmodem_test_data *test_data,
				void *data);

void rilmodem_test_engine_write_socket(struct engine_data *ed,
						const unsigned char *buf,
						const size_t buf_len);

const char *rilmodem_test_engine_get_socket_name(struct engine_data *ed);

void rilmodem_test_engine_next_step(struct engine_data *ed);
const struct rilmodem_test_step *rilmodem_test_engine_get_current_step(
							struct engine_data *ed);

void rilmodem_test_engine_start(struct engine_data *ed);
