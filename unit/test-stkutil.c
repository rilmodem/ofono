/*
 * oFono - GSM Telephony Stack for Linux
 *
 * Copyright (C) 2008-2010 Intel Corporation.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>
#include <glib/gprintf.h>

#include <ofono/types.h>
#include "smsutil.h"
#include "stkutil.h"

static gboolean g_mem_equal(const unsigned char *v1, const unsigned char *v2,
				unsigned int len)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		if (v1[i] != v2[i])
			return FALSE;
	return TRUE;
}

static inline void check_common_bool(const ofono_bool_t command,
					const ofono_bool_t test)
{
	g_assert(command == test);
}

/* Defined in TS 102.223 Section 8.8 */
static void check_duration(const struct stk_duration *command,
					const struct stk_duration *test)
{
	g_assert(command->unit == test->unit);
	g_assert(command->interval == test->interval);
}

/* Defined in TS 102.223 Section 8.15 */
static inline void check_text(const char *command, const char *test)
{
	g_assert(g_str_equal(command, test));
}

/* Defined in TS 102.223 Section 8.31 */
static void check_icon_id(const struct stk_icon_id *command,
					const struct stk_icon_id *test)
{
	g_assert(command->id == test->id);
	g_assert(command->qualifier == test->qualifier);
}

/* Defined in TS 102.223 Section 8.72 */
static void check_text_attr(const struct stk_text_attribute *command,
					const struct stk_text_attribute *test)
{
	g_assert(command->len == test->len);
	g_assert(g_mem_equal(command->attributes, test->attributes, test->len));
}

/* Defined in TS 102.223 Section 8.80 */
static void check_frame_id(const struct stk_frame_id *command,
					const struct stk_frame_id *test)
{
	g_assert(command->has_id == test->has_id);
	if (test->has_id)
		g_assert(command->id == test->id);
}

struct display_text_test {
	const unsigned char *pdu;
	unsigned int pdu_len;
	unsigned char qualifier;
	const char *text;
	struct stk_icon_id icon_id;
	ofono_bool_t immediate_response;
	struct stk_duration duration;
	struct stk_text_attribute text_attribute;
	struct stk_frame_id frame_id;
};

unsigned char display_text_111[] = { 0xD0, 0x1A, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0F,
					0x04, 0x54, 0x6F, 0x6F, 0x6C, 0x6B,
					0x69, 0x74, 0x20, 0x54, 0x65, 0x73,
					0x74, 0x20, 0x31 };

unsigned char display_text_131[] = { 0xD0, 0x1A, 0x81, 0x03, 0x01, 0x21, 0x81,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0F,
					0x04, 0x54, 0x6F, 0x6F, 0x6C, 0x6B,
					0x69, 0x74, 0x20, 0x54, 0x65, 0x73,
					0x74, 0x20, 0x32 };

unsigned char display_text_141[] = { 0xD0, 0x19, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0E,
					0x00, 0xD4, 0xF7, 0x9B, 0xBD, 0x4E,
					0xD3, 0x41, 0xD4, 0xF2, 0x9C, 0x0E,
					0x9A, 0x01 };

unsigned char display_text_151[] = { 0xD0, 0x1A, 0x81, 0x03, 0x01, 0x21, 0x00,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0F,
					0x04, 0x54, 0x6F, 0x6F, 0x6C, 0x6B,
					0x69, 0x74, 0x20, 0x54, 0x65, 0x73,
					0x74, 0x20, 0x34 };

unsigned char display_text_161[] = { 0xD0, 0x81, 0xAD, 0x81, 0x03, 0x01, 0x21,
					0x80, 0x82, 0x02, 0x81, 0x02, 0x8D,
					0x81, 0xA1, 0x04, 0x54, 0x68, 0x69,
					0x73, 0x20, 0x63, 0x6F, 0x6D, 0x6D,
					0x61, 0x6E, 0x64, 0x20, 0x69, 0x6E,
					0x73, 0x74, 0x72, 0x75, 0x63, 0x74,
					0x73, 0x20, 0x74, 0x68, 0x65, 0x20,
					0x4D, 0x45, 0x20, 0x74, 0x6F, 0x20,
					0x64, 0x69, 0x73, 0x70, 0x6C, 0x61,
					0x79, 0x20, 0x61, 0x20, 0x74, 0x65,
					0x78, 0x74, 0x20, 0x6D, 0x65, 0x73,
					0x73, 0x61, 0x67, 0x65, 0x2E, 0x20,
					0x49, 0x74, 0x20, 0x61, 0x6C, 0x6C,
					0x6F, 0x77, 0x73, 0x20, 0x74, 0x68,
					0x65, 0x20, 0x53, 0x49, 0x4D, 0x20,
					0x74, 0x6F, 0x20, 0x64, 0x65, 0x66,
					0x69, 0x6E, 0x65, 0x20, 0x74, 0x68,
					0x65, 0x20, 0x70, 0x72, 0x69, 0x6F,
					0x72, 0x69, 0x74, 0x79, 0x20, 0x6F,
					0x66, 0x20, 0x74, 0x68, 0x61, 0x74,
					0x20, 0x6D, 0x65, 0x73, 0x73, 0x61,
					0x67, 0x65, 0x2C, 0x20, 0x61, 0x6E,
					0x64, 0x20, 0x74, 0x68, 0x65, 0x20,
					0x74, 0x65, 0x78, 0x74, 0x20, 0x73,
					0x74, 0x72, 0x69, 0x6E, 0x67, 0x20,
					0x66, 0x6F, 0x72, 0x6D, 0x61, 0x74,
					0x2E, 0x20, 0x54, 0x77, 0x6F, 0x20,
					0x74, 0x79, 0x70, 0x65, 0x73, 0x20,
					0x6F, 0x66, 0x20, 0x70, 0x72, 0x69,
					0x6F };

unsigned char display_text_171[] = { 0xD0, 0x1A, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0F,
					0x04, 0x3C, 0x47, 0x4F, 0x2D, 0x42,
					0x41, 0x43, 0x4B, 0x57, 0x41, 0x52,
					0x44, 0x53, 0x3E };

unsigned char display_text_511[] = { 0xD0, 0x1A, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0B,
					0x04, 0x42, 0x61, 0x73, 0x69, 0x63,
					0x20, 0x49, 0x63, 0x6F, 0x6E, 0x9E,
					0x02, 0x00, 0x01 };

unsigned char display_text_521[] = { 0xD0, 0x1B, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0C,
					0x04, 0x43, 0x6F, 0x6C, 0x6F, 0x75,
					0x72, 0x20, 0x49, 0x63, 0x6F, 0x6E,
					0x9E, 0x02, 0x00, 0x02 };

unsigned char display_text_531[] = { 0xD0, 0x1A, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0B,
					0x04, 0x42, 0x61, 0x73, 0x69, 0x63,
					0x20, 0x49, 0x63, 0x6F, 0x6E, 0x9E,
					0x02, 0x01, 0x01 };

unsigned char display_text_611[] = { 0xD0, 0x24, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x19,
					0x08, 0x04, 0x17, 0x04, 0x14, 0x04,
					0x20, 0x04, 0x10, 0x04, 0x12, 0x04,
					0x21, 0x04, 0x22, 0x04, 0x12, 0x04,
					0x23, 0x04, 0x19, 0x04, 0x22, 0x04,
					0x15 };

unsigned char display_text_711[] = { 0xD0, 0x19, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x0A,
					0x04, 0x31, 0x30, 0x20, 0x53, 0x65,
					0x63, 0x6F, 0x6E, 0x64, 0x84, 0x02,
					0x01, 0x0A };

unsigned char display_text_911[] = { 0xD0, 0x10, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x05,
					0x08, 0x4F, 0x60, 0x59, 0x7D };

unsigned char display_text_1011[] = { 0xD0, 0x12, 0x81, 0x03, 0x01, 0x21, 0x80,
					0x82, 0x02, 0x81, 0x02, 0x8D, 0x07,
					0x08, 0x00, 0x38, 0x00, 0x30, 0x30,
					0xEB };

static struct display_text_test display_text_data_111 = {
	.pdu = display_text_111,
	.pdu_len = sizeof(display_text_111),
	.qualifier = 0x80,
	.text = "Toolkit Test 1"
};

static struct display_text_test display_text_data_131 = {
	.pdu = display_text_131,
	.pdu_len = sizeof(display_text_131),
	.qualifier = 0x81,
	.text = "Toolkit Test 2"
};

static struct display_text_test display_text_data_141 = {
	.pdu = display_text_141,
	.pdu_len = sizeof(display_text_141),
	.qualifier = 0x80,
	.text = "Toolkit Test 3"
};

static struct display_text_test display_text_data_151 = {
	.pdu = display_text_151,
	.pdu_len = sizeof(display_text_151),
	.qualifier = 0x00,
	.text = "Toolkit Test 4"
};

static struct display_text_test display_text_data_161 = {
	.pdu = display_text_161,
	.pdu_len = sizeof(display_text_161),
	.qualifier = 0x80,
	.text = "This command instructs the ME to display a text message. "
			"It allows the SIM to define the priority of that "
			"message, and the text string format. Two types of "
			"prio"
};

static struct display_text_test display_text_data_171 = {
	.pdu = display_text_171,
	.pdu_len = sizeof(display_text_171),
	.qualifier = 0x80,
	.text = "<GO-BACKWARDS>"
};

static struct display_text_test display_text_data_511 = {
	.pdu = display_text_511,
	.pdu_len = sizeof(display_text_511),
	.qualifier = 0x80,
	.text = "Basic Icon",
	.icon_id = {
		.qualifier = 0x00,
		.id = 0x01
	}
};

static struct display_text_test display_text_data_521 = {
	.pdu = display_text_521,
	.pdu_len = sizeof(display_text_521),
	.qualifier = 0x80,
	.text = "Colour Icon",
	.icon_id = {
		.qualifier = 0x00,
		.id = 0x02
	}
};

static struct display_text_test display_text_data_531 = {
	.pdu = display_text_531,
	.pdu_len = sizeof(display_text_531),
	.qualifier = 0x80,
	.text = "Basic Icon",
	.icon_id = {
		.qualifier = 0x01,
		.id = 0x01
	}
};

static struct display_text_test display_text_data_611 = {
	.pdu = display_text_611,
	.pdu_len = sizeof(display_text_611),
	.qualifier = 0x80,
	.text = "ЗДРАВСТВУЙТЕ"
};

static struct display_text_test display_text_data_711 = {
	.pdu = display_text_711,
	.pdu_len = sizeof(display_text_711),
	.qualifier = 0x80,
	.text = "10 Second",
	.duration = {
		.unit = STK_DURATION_TYPE_SECONDS,
		.interval = 10,
	}
};

static struct display_text_test display_text_data_911 = {
	.pdu = display_text_911,
	.pdu_len = sizeof(display_text_911),
	.qualifier = 0x80,
	.text = "你好"
};

static struct display_text_test display_text_data_1011 = {
	.pdu = display_text_1011,
	.pdu_len = sizeof(display_text_1011),
	.qualifier = 0x80,
	.text = "80ル"
};

/* Defined in TS 102.384 Section 27.22.4.1 */
static void test_display_text(gconstpointer data)
{
	const struct display_text_test *test = data;
	struct stk_command *command;

	command = stk_command_new_from_pdu(test->pdu, test->pdu_len);

	g_assert(command);

	g_assert(command->number == 1);
	g_assert(command->type == STK_COMMAND_TYPE_DISPLAY_TEXT);
	g_assert(command->qualifier == test->qualifier);

	g_assert(command->src == STK_DEVICE_IDENTITY_TYPE_UICC);
	g_assert(command->dst == STK_DEVICE_IDENTITY_TYPE_DISPLAY);

	g_assert(command->display_text.text);
	check_text(command->display_text.text, test->text);
	check_icon_id(&command->display_text.icon_id, &test->icon_id);
	check_common_bool(command->display_text.immediate_response,
						test->immediate_response);
	check_duration(&command->display_text.duration, &test->duration);
	check_text_attr(&command->display_text.text_attribute,
						&test->text_attribute);
	check_frame_id(&command->display_text.frame_id, &test->frame_id);

	stk_command_free(command);
}

struct get_input_test {
	const unsigned char *pdu;
	unsigned int pdu_len;
	unsigned char qualifier;
	const char *expected;
	unsigned char min;
	unsigned char max;
	unsigned char icon_qualifier;
	unsigned char icon_id;
};

static unsigned char get_input_111[] = { 0xD0, 0x1B, 0x81, 0x03, 0x01, 0x23,
						0x00, 0x82, 0x02, 0x81, 0x82,
						0x8D, 0x0C, 0x04, 0x45, 0x6E,
						0x74, 0x65, 0x72, 0x20, 0x31,
						0x32, 0x33, 0x34, 0x35, 0x91,
						0x02, 0x05, 0x05 };

static struct get_input_test get_input_data_111 = {
	.pdu = get_input_111,
	.pdu_len = sizeof(get_input_111),
	.expected = "Enter 12345",
	.qualifier = 0x00,
	.min = 5,
	.max = 5
};

/* Defined in TS 102.384 Section 27.22.4.3 */
static void test_get_input(gconstpointer data)
{
	const struct get_input_test *test = data;
	struct stk_command *command;

	command = stk_command_new_from_pdu(test->pdu, test->pdu_len);

	g_assert(command);

	g_assert(command->number == 1);
	g_assert(command->type == STK_COMMAND_TYPE_GET_INPUT);
	g_assert(command->qualifier == test->qualifier);

	g_assert(command->src == STK_DEVICE_IDENTITY_TYPE_UICC);
	g_assert(command->dst == STK_DEVICE_IDENTITY_TYPE_TERMINAL);

	g_assert(command->get_input.text);

	g_assert(g_str_equal(test->expected, command->get_input.text));

	g_assert(command->get_input.response_length.min == test->min);
	g_assert(command->get_input.response_length.max == test->max);

	if (test->icon_id > 0) {
		g_assert(command->get_input.icon_id.id == test->icon_id);
		g_assert(command->get_input.icon_id.qualifier ==
				test->icon_qualifier);
	}

	stk_command_free(command);
}

struct send_sms_test {
	const unsigned char *pdu;
	unsigned int pdu_len;
	unsigned char qualifier;
	const char *alpha_id;
	unsigned char ton_npi;
	const char *address;
	unsigned char sms_mr;
	const char *sms_address;
	unsigned char sms_udl;
	const char *sms_ud;
};

/* 3GPP TS 31.124 Section 27.22.4.10.1.4.2 */
static unsigned char send_sms_11[] = { 0xD0, 0x37, 0x81, 0x03, 0x01, 0x13, 0x00,
					0x82, 0x02, 0x81, 0x83, 0x85, 0x07,
					0x53, 0x65, 0x6E, 0x64, 0x20, 0x53,
					0x4D, 0x86, 0x09, 0x91, 0x11, 0x22,
					0x33, 0x44, 0x55, 0x66, 0x77, 0xF8,
					0x8B, 0x18, 0x01, 0x00, 0x09, 0x91,
					0x10, 0x32, 0x54, 0x76, 0xF8, 0x40,
					0xF4, 0x0C, 0x54, 0x65, 0x73, 0x74,
					0x20, 0x4D, 0x65, 0x73, 0x73, 0x61,
					0x67, 0x65 };

static struct send_sms_test send_sms_data_11 = {
	.pdu = send_sms_11,
	.pdu_len = sizeof(send_sms_11),
	.qualifier = 0x00,
	.alpha_id = "Send SM",
	.ton_npi = 0x91,
	.address = "112233445566778",
	.sms_mr = 0x00,
	.sms_address = "012345678",
	.sms_udl = 12,
	.sms_ud = "Test Message",
};

static void test_send_sms(gconstpointer data)
{
	const struct send_sms_test *test = data;
	struct stk_command *command;
	int i;

	command = stk_command_new_from_pdu(test->pdu, test->pdu_len);

	g_assert(command);

	g_assert(command->number == 1);
	g_assert(command->type == STK_COMMAND_TYPE_SEND_SMS);
	g_assert(command->qualifier == test->qualifier);

	g_assert(command->src == STK_DEVICE_IDENTITY_TYPE_UICC);
	g_assert(command->dst == STK_DEVICE_IDENTITY_TYPE_NETWORK);

	if (test->alpha_id)
		g_assert(g_str_equal(test->alpha_id,
					command->send_sms.alpha_id));

	if (test->address) {
		g_assert(test->ton_npi == command->send_sms.address.ton_npi);
		g_assert(g_str_equal(test->address,
					command->send_sms.address.number));
	}

	g_assert(test->sms_mr == command->send_sms.gsm_sms.submit.mr);
	g_assert(test->sms_udl == command->send_sms.gsm_sms.submit.udl);
	g_assert(g_str_equal(test->sms_address,
			command->send_sms.gsm_sms.submit.daddr.address));

	for (i = 0; i < test->sms_udl; i++)
		g_assert(test->sms_ud[i] ==
				command->send_sms.gsm_sms.submit.ud[i]);

	stk_command_free(command);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_data_func("/teststk/Display Text 1.1.1",
				&display_text_data_111, test_display_text);
	g_test_add_data_func("/teststk/Display Text 1.3.1",
				&display_text_data_131, test_display_text);
	g_test_add_data_func("/teststk/Display Text 1.4.1",
				&display_text_data_141, test_display_text);
	g_test_add_data_func("/teststk/Display Text 1.5.1",
				&display_text_data_151, test_display_text);
	g_test_add_data_func("/teststk/Display Text 1.6.1",
				&display_text_data_161, test_display_text);
	g_test_add_data_func("/teststk/Display Text 1.7.1",
				&display_text_data_171, test_display_text);
	g_test_add_data_func("/teststk/Display Text 5.1.1",
				&display_text_data_511, test_display_text);
	g_test_add_data_func("/teststk/Display Text 5.2.1",
				&display_text_data_521, test_display_text);
	g_test_add_data_func("/teststk/Display Text 5.3.1",
				&display_text_data_531, test_display_text);
	g_test_add_data_func("/teststk/Display Text 6.1.1",
				&display_text_data_611, test_display_text);
	g_test_add_data_func("/teststk/Display Text 7.1.1",
				&display_text_data_711, test_display_text);
	g_test_add_data_func("/teststk/Display Text 9.1.1",
				&display_text_data_911, test_display_text);
	g_test_add_data_func("/teststk/Display Text 10.1.1",
				&display_text_data_1011, test_display_text);

	g_test_add_data_func("/teststk/Get Input 1.1.1",
				&get_input_data_111, test_get_input);

	g_test_add_data_func("/teststk/Send SMS 1.1",
				&send_sms_data_11, test_send_sms);

	return g_test_run();
}
