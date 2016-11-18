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

/*
 * OFONO_EVENT_CALL_* macros are used to implement ofono atom functions that
 * get called by the rilmodem driver. The macros make sure that the call was
 * performed at the right step and call a check function that is specific to
 * each step and that can be used to check if the function arguments are as
 * expected.
 */

#define OFONO_EVENT_CALL_ARG_1(func, type1)				\
void func(type1 v1)							\
{									\
	const struct rilmodem_test_step *step;				\
									\
	step = rilmodem_test_engine_get_current_step(v1->engined); 	\
									\
	g_assert(step->type == TST_EVENT_CALL);				\
	g_assert(step->call_func == (void (*)(void)) (func));		\
									\
	if (step->check_func != NULL)					\
		((void (*)(type1)) step->check_func)(v1);		\
									\
	rilmodem_test_engine_next_step(v1->engined);			\
}

#define OFONO_EVENT_CALL_ARG_2(func, type1, type2)			\
void func(type1 v1, type2 v2)						\
{									\
	const struct rilmodem_test_step *step;				\
									\
	step = rilmodem_test_engine_get_current_step(v1->engined); 	\
									\
	g_assert(step->type == TST_EVENT_CALL);				\
	g_assert(step->call_func == (void (*)(void)) (func));		\
									\
	if (step->check_func != NULL)					\
		((void (*)(type1, type2)) step->check_func)(v1, v2);	\
									\
	rilmodem_test_engine_next_step(v1->engined);			\
}

#define OFONO_EVENT_CALL_ARG_3(func, type1, type2, type3)		\
void func(type1 v1, type2 v2, type3 v3)					\
{									\
	const struct rilmodem_test_step *step;				\
									\
	step = rilmodem_test_engine_get_current_step(v1->engined); 	\
									\
	g_assert(step->type == TST_EVENT_CALL);				\
	g_assert(step->call_func == (void (*)(void)) (func));		\
									\
	if (step->check_func != NULL)					\
		((void (*)(type1, type2, type3))			\
					step->check_func)(v1, v2, v3);	\
									\
	rilmodem_test_engine_next_step(v1->engined);			\
}

#define OFONO_EVENT_CALL_CB_ARG_2(func, type1, type2)			\
static void func(type1 v1, void *data)					\
{									\
	type2 v2 = data;						\
	const struct rilmodem_test_step *step;				\
									\
	step = rilmodem_test_engine_get_current_step(v2->engined); 	\
									\
	g_assert(step->type == TST_EVENT_CALL);				\
	g_assert(step->call_func == (void (*)(void)) (func));		\
									\
	if (step->check_func != NULL)					\
		((void (*)(type1, type2)) step->check_func)(v1, v2);	\
									\
	rilmodem_test_engine_next_step(v2->engined);			\
}

#define OFONO_EVENT_CALL_CB_ARG_3(func, type1, type2, type3)		\
static void func(type1 v1, type2 v2, void *data)			\
{									\
	type3 v3 = data;						\
	const struct rilmodem_test_step *step;				\
									\
	step = rilmodem_test_engine_get_current_step(v3->engined); 	\
									\
	g_assert(step->type == TST_EVENT_CALL);				\
	g_assert(step->call_func == (void (*)(void)) (func));		\
									\
	if (step->check_func != NULL)					\
		((void (*)(type1, type2, type3))			\
					step->check_func)(v1, v2, v3);	\
									\
	rilmodem_test_engine_next_step(v3->engined);			\
}

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
