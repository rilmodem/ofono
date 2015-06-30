/*
 *
 *  DNS Client - provides an asynchronous DNS resolution client.
 *
 *  This file originally created by Google, Inc.
 *  Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
 *  Copyright (c) 2015 Canonical Ltd.
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

#ifndef OFONO_DNS_CLIENT_H
#define OFONO_DNS_CLIENT_H

struct sockaddr;

/**
 * SECTION:dns_client
 * @title: DNS client interface
 * @short_description: Functions for interacting with asynchronous DNS client
 */

/*
 * Enumeration of possible DNS request completion status codes.
 * These are intentionally consistent with shill error codes.
 */
typedef enum {
	OFONO_DNS_CLIENT_SUCCESS = 0,
	OFONO_DNS_CLIENT_ERROR_NO_DATA,
	OFONO_DNS_CLIENT_ERROR_FORM_ERR,
	OFONO_DNS_CLIENT_ERROR_SERVER_FAIL,
	OFONO_DNS_CLIENT_ERROR_NOT_FOUND,
	OFONO_DNS_CLIENT_ERROR_NOT_IMP,
	OFONO_DNS_CLIENT_ERROR_REFUSED,
	OFONO_DNS_CLIENT_ERROR_BAD_QUERY,
	OFONO_DNS_CLIENT_ERROR_NET_REFUSED,
	OFONO_DNS_CLIENT_ERROR_TIMED_OUT,
	OFONO_DNS_CLIENT_ERROR_UNKNOWN,
} ofono_dns_client_status_t;

/*
 * Callbacks of this type are provided by a client when initiating a DNS request
 * and invoked by the dns_client module when a request has completed.
 * |data| is user data supplied by the client.
 * |status| is the completion status of the request.
 * |ip_addr| is a struct sockaddr containing an  IPv4 or IPv6 address if the
 * request succeeded and NULL otherwise. The callee should copy |ip_addr| if it
 * wishes to retain it beyond the lifetime of this callback.
 */
typedef void (*ofono_dns_client_callback_t)(void *data,
					ofono_dns_client_status_t status,
					struct sockaddr *ip_addr);

/*
 * Opaque handle uniquely indentifying an in-progress request.
 */
typedef void *ofono_dns_client_request_t;

struct ofono_dns_client_driver {
	const char *name;
	ofono_dns_client_request_t (*submit_request)(const char *hostname,
						const char *device,
						const char **servers,
						int timeout_ms,
						ofono_dns_client_callback_t cb,
						void *data);
	gboolean (*cancel_request)(ofono_dns_client_request_t request);
};


/*
 * Initiate an asynchronous name resolution request for |hostname|.
 * |device| specifies the interface out which the request should be sent, or
 * NULL to use the default interface.
 * |servers| string array with name servers to query or NULL to use system
 * defaults
 * |timeout_ms| specifies a max timeout in milliseconds, or 0 to use the default
 * timeout (which depends on the plugin).
 * Returns an opaque handle identifying the request on success and NULL on
 * failure. Success indicates only that the request was initiated, not that the
 * request itself was successful.
 * When the request is complete, the callback |cb| will be invoked with |data|
 * passed as an arg.
 */
ofono_dns_client_request_t __ofono_dns_client_submit_request(
						const char *hostname,
						const char *device,
						const char **servers,
						int timeout_ms,
						ofono_dns_client_callback_t cb,
						void *data);

/*
 * Cancel the request identified by |request|. Does not invoke the callback for
 * the cancelled request.
 */
void __ofono_dns_client_cancel_request(ofono_dns_client_request_t request);

/*
 * Returns a human-friendly error string corresponding to |status|.
 */
const char *__ofono_dns_client_strerror(ofono_dns_client_status_t status);

int ofono_dns_client_driver_register(struct ofono_dns_client_driver *driver);
void ofono_dns_client_driver_unregister(
				const struct ofono_dns_client_driver *driver);

#endif /* OFONO_DNS_CLIENT_H */
