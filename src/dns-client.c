/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
 *  Copyright (C) 2015  Canonical Ltd.
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
#include <string.h>
#include <glib.h>
#include "ofono.h"
#include "dns-client.h"

static GSList *g_drivers;

ofono_dns_client_request_t __ofono_dns_client_submit_request(
						const char *hostname,
						const char *device,
						const char **servers,
						int timeout_ms,
						ofono_dns_client_callback_t cb,
						void *data)
{
	GSList *d;
	ofono_dns_client_request_t token = NULL;

	for (d = g_drivers; d != NULL; d = d->next) {
		const struct ofono_dns_client_driver *driver = d->data;

		if (driver->submit_request == NULL)
			continue;

		DBG("Calling dns client plugin '%s'", driver->name);

		token = driver->submit_request(hostname, device, servers,
							timeout_ms, cb, data);
		if (token == NULL)
			continue;

		return token;
	}

	return token;
}

void __ofono_dns_client_cancel_request(ofono_dns_client_request_t request)
{
	GSList *d;

	for (d = g_drivers; d != NULL; d = d->next) {
		const struct ofono_dns_client_driver *driver = d->data;

		if (driver->cancel_request == NULL)
			continue;

		/* Returns TRUE if this driver is the request owner */
		if (driver->cancel_request(request))
			break;
	}
}

/*
 * Returns a human-friendly error string corresponding to |status|.
 * The strings that we return are intentionally consistent with shill error
 * messages.
 */
const char *__ofono_dns_client_strerror(ofono_dns_client_status_t status)
{
	switch (status) {
	case OFONO_DNS_CLIENT_SUCCESS:
		return "The query was successful.";
	case OFONO_DNS_CLIENT_ERROR_NO_DATA:
		return "The query response contains no answers.";
	case OFONO_DNS_CLIENT_ERROR_FORM_ERR:
		return "The server says the query is bad.";
	case OFONO_DNS_CLIENT_ERROR_SERVER_FAIL:
		return "The server says it had a failure.";
	case OFONO_DNS_CLIENT_ERROR_NOT_FOUND:
		return "The queried-for domain was not found.";
	case OFONO_DNS_CLIENT_ERROR_NOT_IMP:
		return "The server doesn't implement operation.";
	case OFONO_DNS_CLIENT_ERROR_REFUSED:
		return "The server replied, refused the query.";
	case OFONO_DNS_CLIENT_ERROR_BAD_QUERY:
		return "Locally we could not format a query.";
	case OFONO_DNS_CLIENT_ERROR_NET_REFUSED:
		return "The network connection was refused.";
	case OFONO_DNS_CLIENT_ERROR_TIMED_OUT:
		return "The network connection was timed out.";
	case OFONO_DNS_CLIENT_ERROR_UNKNOWN:
	default:
		return "DNS Resolver unknown internal error.";
	}
}

int ofono_dns_client_driver_register(struct ofono_dns_client_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	g_drivers = g_slist_prepend(g_drivers, driver);
	return 0;
}

void ofono_dns_client_driver_unregister(
			const struct ofono_dns_client_driver *driver)
{
	DBG("driver: %p name: %s", driver, driver->name);

	g_drivers = g_slist_remove(g_drivers, driver);
}
