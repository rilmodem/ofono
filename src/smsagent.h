/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010  Nokia Corporation and/or its subsidiary(-ies).
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

struct sms_agent;

enum sms_agent_result {
	SMS_AGENT_RESULT_OK = 0,
	SMS_AGENT_RESULT_FAILED,
	SMS_AGENT_RESULT_TIMEOUT,
};

typedef void (*sms_agent_dispatch_cb)(struct sms_agent *agent,
					enum sms_agent_result result,
					void *data);

struct sms_agent *sms_agent_new(const char *interface,
					const char *service, const char *path);

void sms_agent_set_removed_notify(struct sms_agent *agent,
					ofono_destroy_func destroy,
					void *user_data);

ofono_bool_t sms_agent_matches(struct sms_agent *agent, const char *service,
				const char *path);

void sms_agent_free(struct sms_agent *agent);

int sms_agent_dispatch_datagram(struct sms_agent *agent, const char *method,
				const char *from,
				const struct tm *remote_sent_time,
				const struct tm *local_sent_time,
				const unsigned char *content, unsigned int len,
				sms_agent_dispatch_cb cb, void *user_data,
				ofono_destroy_func destroy);
