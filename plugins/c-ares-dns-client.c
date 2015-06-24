/*
 *
 *  DNS Client - provides an asynchronous DNS resolution client.
 *
 *  The client is implemented using the c-ares library, and integrates with
 *  glib's main event loop. It was originally written to be used by flimflam,
 *  and it has been modified to integrate with ofono.  See http://c-ares.haxx.se
 *  and http://developer.gnome.org/glib for c-ares and glib documentation.
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/time.h>

#include <ares.h>
#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/types.h>
#include <ofono/log.h>
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/dns-client.h>

/* Structure representing a pending asynchronous name resolution request. */
struct ares_request {
	char *hostname;  /* hostname that we're resolving */
	char *interface; /* interface to use for queries */
	struct ares_addr_node *servers;
	struct timeval timeout;  /* caller-specified timeout */
	struct timeval start_time;  /* time at which request was started */
	ofono_dns_client_callback_t cb;  /* client-provided callback */
	void *data;  /* user data */
	ares_channel channel;  /* opaque, used by c-ares library */
	GHashTable *ares_watches;  /* fds that we're monitoring for c-ares */
	guint timeout_source_id;  /* glib source id for our ares timeout */
	gboolean running;  /* stopped requests are eligible for deletion */
};

/*
 * Structure representing a file descriptor that we're monitoring within our
 * glib event loop for c-ares.
 */
struct ares_watch {
	struct ares_request *request;  /* backpointer to our owner */
	int fd;	 /* file descriptor that we're watching */
	GIOChannel *gio_channel;  /* glib IO channel */
	GIOCondition gio_condition;  /* events in which we're interested */
	guint g_source_id;  /* glib source id */
};

/*
 * List of pending asynchronous name resolution requests. We expect the number
 * of pending requests to be small, hence the use of a linked list.
 */
static GList *pending_requests = NULL;

/*
 * ares requests are often stopped from within ares callbacks. In these cases,
 * we defer deletion of the ares_request struct to the idle loop. This is the
 * glib source id associated with the deferred deletion task.
 */
static guint deferred_deletion_g_source_id = 0;

static void reset_ares_timeout(struct ares_request *request,
			       gboolean destroy_old_source);
static void stop_ares_request(struct ares_request *request);

/*#define USE_RP_FILTER*/

/*
 * Set/unset reverse path filtering (from connman). Using this should make
 * setting routes for the DNS servers unnecessary, but unfortunately that is not
 * happening.
 * TODO Investigate why this is not working.
 */
#ifdef USE_RP_FILTER
static void rp_filter_set(const char *interface, gboolean enabled)
{
	int fd;
	ssize_t cnt;
	char filename[PATH_MAX];
	const char *str_value = (enabled == TRUE) ? "1" : "0";

	snprintf(filename, sizeof(filename),
			"/proc/sys/net/ipv4/conf/%s/rp_filter", interface);
	fd = open(filename, O_WRONLY);
	if (fd == -1)
		return;
	cnt = write(fd, str_value, strlen(str_value));
	if (cnt == -1)
		ofono_error("%s: cannot write (%s)", __func__, strerror(errno));
	close(fd);
}
#else
static void rp_filter_set(const char *interface, gboolean enabled)
{
}
#endif

/*
 * Callback invoked when it's time to give control back to c-ares. Controlled by
 * the glib source referred to by |timeout_source_id| in struct ares_request.
 */
static gboolean ares_timeout_cb(gpointer data)
{
	struct ares_request *request = data;
	const gboolean destroy_old_source = FALSE;

	DBG("request %p: running = %d", request, request->running);

	if (!request->running) {
		request->timeout_source_id = 0;
		return FALSE;
	}

	ares_process_fd(request->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);

	/*
	 * NOTE: We tell reset_ares_timeout not to destroy its old timer source
	 * because we're calling it from within that source and it will be
	 * destroyed by glib when we return FALSE below.
	 */
	reset_ares_timeout(request, destroy_old_source);

	/*
	 * Return FALSE to get rid of our old glib source. We created a new
	 * one during our call to reset_ares_timeout above.
	 */
	return FALSE;
}

/*
 * Determine how long c-ares is willing to wait until being given control and
 * schedule ares_timeout_cb to be invoked at that time. Any existing
 * timer is replaced. If |destroy_old_source| is TRUE, the old timer's glib
 * source will be destroyed.
 */
static void reset_ares_timeout(struct ares_request *request,
			       gboolean destroy_old_source)
{
	struct timeval ret_tv, now, elapsed, max_tv;
	struct timeval *tv;
	struct timeval *max = NULL;
	guint timeout_interval_msecs = 0;
	gboolean timeout_provided = FALSE;

	DBG("request %p: running = %d", request, request->running);

	if (!request->running)
		return;

	/*
	 * Compute how much time has elapsed since the request started.
	 * If the client provided a non-default timeout and we've timed out,
	 * notify the client and stop the request.
	 */
	gettimeofday(&now, NULL);
	timersub(&now, &request->start_time, &elapsed);
	timeout_provided = request->timeout.tv_sec != 0 ||
			   request->timeout.tv_usec != 0;
	if (timeout_provided && timercmp(&elapsed, &request->timeout, >=)) {
		request->cb(request->data, OFONO_DNS_CLIENT_ERROR_TIMED_OUT,
			    NULL);
		stop_ares_request(request);
		return;
	}

	/*
	 * Tell c-ares how long we're willing to wait (max) and see if it wants
	 * to regain control sooner than that.
	 */
	if (timeout_provided) {
		timersub(&request->timeout, &elapsed, &max_tv);
		max = &max_tv;
	}
	if ((tv = ares_timeout(request->channel, max, &ret_tv)) == NULL) {
		ofono_error("%s: ares_timeout failed", __func__);
		return;
	}

	/*
	 * Reschedule our timeout to be the sooner of the ares-specified tiemout
	 * and the client-specified timeout.
	 */
	if (request->timeout_source_id != 0 && destroy_old_source) {
		if (!g_source_remove(request->timeout_source_id))
			ofono_error("%s: g_source_remove failed", __func__);
	}

	timeout_interval_msecs = tv->tv_sec * 1000 + tv->tv_usec / 1000;
	DBG("timeout interval = %u", timeout_interval_msecs);

	request->timeout_source_id = g_timeout_add(timeout_interval_msecs,
							ares_timeout_cb,
							request);
}

/*
 * Callback invoked by glib when there is activity on a file descriptor that
 * we're monitoring for c-ares.
 */
static gboolean ares_watch_io_cb(GIOChannel *source,
				 GIOCondition condition,
				 gpointer data)
{
	struct ares_watch *watch = data;
	ares_socket_t read_fd = ARES_SOCKET_BAD;
	ares_socket_t write_fd = ARES_SOCKET_BAD;
	const gboolean destroy_old_source = TRUE;

	DBG("watch %p (fd %d): condition = 0x%x", watch, watch->fd,
			condition);

	if (!watch->request->running) {
		/* Destroy this source by returning FALSE. */
		watch->g_source_id = 0;
		return FALSE;
	}

	if (condition & (G_IO_NVAL | G_IO_HUP | G_IO_ERR)) {
		ofono_error("%s: error condition on fd %d", __func__,
			      watch->fd);
		watch->g_source_id = 0;
		return FALSE;
	}

	if (condition & G_IO_IN)
		read_fd = watch->fd;
	if (condition & G_IO_OUT)
		write_fd = watch->fd;

	/* Give control to c-ares. */
	ares_process_fd(watch->request->channel, read_fd, write_fd);

	reset_ares_timeout(watch->request, destroy_old_source);

	return TRUE;
}

/*
 * Destroy an ares_watch structure. We register this as our value destroy
 * function when creating the ares_watches table, and it is called by glib
 * whenever we remove a value from the table or destroy the table.
 */
static void destroy_ares_watch(gpointer data)
{
	struct ares_watch *watch = data;

	DBG("watch %p (fd %d)", watch, watch->fd);

	if (watch->g_source_id != 0) {
		if (!g_source_remove(watch->g_source_id)) {
			ofono_error("%s: g_source_remove failed for id %d",
					__func__, watch->g_source_id);
		}
		watch->g_source_id = 0;
	}

	g_io_channel_unref(watch->gio_channel);
	g_free(watch);
}

/*
 * Create an ares_watch for |fd| and store it in the ares_watches table for
 * |request|. Monitor for readability if |read| is TRUE. Monitor for writability
 * if |write| is TRUE. If there is already an entry for |fd| in the table,
 * update it according to the values of |read| and |write|.
 */
static gboolean init_ares_watch(struct ares_request *request, int fd,
				gboolean read, gboolean write)
{
	struct ares_watch *watch;

	DBG("fd = %d, read = %d, write = %d", fd, read, write);

	/*
	 * If there's an old watch in the table, destroy it. We'll replace it
	 * with a new one below if c-ares is still interested in this fd.
	 */
	if (g_hash_table_lookup(request->ares_watches, &fd) != NULL) {
		/* This removal calls destroy_ares_watch on the old watch. */
		g_hash_table_remove(request->ares_watches, &fd);
	}

	if (!read && !write)
		return TRUE;

	watch = g_malloc0(sizeof(struct ares_watch));

	watch->request = request;
	watch->fd = fd;
	watch->g_source_id = 0;

	watch->gio_condition = G_IO_NVAL | G_IO_HUP | G_IO_ERR;
	if (read)
		watch->gio_condition |= G_IO_IN;
	if (write)
		watch->gio_condition |= G_IO_OUT;

	watch->gio_channel = g_io_channel_unix_new(fd);
	if (watch->gio_channel == NULL) {
		ofono_error("%s: could not create g_io_channel for fd %d",
								__func__, fd);
		g_free(watch);
		return FALSE;
	}
	g_io_channel_set_close_on_unref(watch->gio_channel, FALSE);

	g_hash_table_insert(request->ares_watches, &fd, watch);

	watch->g_source_id = g_io_add_watch(watch->gio_channel,
					    watch->gio_condition,
					    ares_watch_io_cb,
					    watch);

	return TRUE;
}

/*
 * Destroy an ares_request struct, freeing the resources allocated in
 * init_ares_request. |request| must already have been removed from the
 * |pending_requests| list and must have been marked not running.
 */
static void destroy_ares_request(struct ares_request *request)
{
	struct ares_addr_node *node, *next;

	DBG("request %p", request);

	ares_destroy(request->channel);
	g_free(request->hostname);
	if (request->interface) {
		rp_filter_set(request->interface, TRUE);
		g_free(request->interface);
	}
	for (node = request->servers; node != NULL; node = next) {
		next = node->next;
		g_free(node);
	}
	if (request->timeout_source_id != 0)
		g_source_remove(request->timeout_source_id);
	/* Hash table destruction calls destroy_ares_watch on all watches. */
	g_hash_table_destroy(request->ares_watches);
	g_free(request);
}

/*
 * Callback invoked from the main loop to perform deferred deletion of stopped
 * ares_request objects. We do deferred deletion to avoid problems when we're in
 * an ares callback and want to delete an object that contains context
 * associated with that callback.
 */
static gboolean delete_stopped_ares_requests_cb(gpointer data)
{
	GList *node, *next;
	struct ares_request *request;
	guint num_requests_deleted = 0;

	DBG("pending_requests list has length %u",
			g_list_length(pending_requests));

	/*
	 * Inspect each request in |pending_requests| and destroy it if it's
	 * not running.
	 */
	for (node = pending_requests; node != NULL; node = next) {
		next = g_list_next(node);
		request = node->data;
		if (!request->running) {
			pending_requests = g_list_delete_link(pending_requests,
							      node);
			destroy_ares_request(request);
			++num_requests_deleted;
		}
	}
	DBG("deleted %u stopped requests", num_requests_deleted);

	deferred_deletion_g_source_id = 0;
	return FALSE;
}

/*
 * Stop an ares_request and schedule the deferred deletion task if it's
 * not already running.
 */
static void stop_ares_request(struct ares_request *request)
{
	DBG("");

	request->running = FALSE;

	if (deferred_deletion_g_source_id != 0)
		return;

	deferred_deletion_g_source_id =
	    g_idle_add(delete_stopped_ares_requests_cb, NULL);
	if (deferred_deletion_g_source_id == 0)
		ofono_error("%s: g_idle_add failed", __func__);
}

/*
 * Callback that is invoked by c-ares to tell us which sockets it wants us to
 * monitor for readability and writability.
 */
static void ares_socket_state_cb(void *data, int s, int read, int write)
{
	struct ares_request *request = (struct ares_request *) data;

	DBG("");

	if (!request->running)
		return;

	DBG("socket %d: read = %d, write = %d", s, read, write);

	if (!init_ares_watch(request, s, read, write))
		ofono_error("%s: couldn't create ares_watch for socket %d",
			      __func__, s);
}

/*
 * Converts a c-ares status code to the corresponding dns_client status code.
 * We do this to completely encapsulate c-ares. In theory, we should be able to
 * replace it with a different asynchronous DNS library without changing our
 * clients.
 */
static ofono_dns_client_status_t status_from_ares_status(int ares_status)
{
	switch(ares_status) {
	case ARES_SUCCESS:
		return OFONO_DNS_CLIENT_SUCCESS;
	case ARES_ENODATA:
		return OFONO_DNS_CLIENT_ERROR_NO_DATA;
	case ARES_EFORMERR:
		return OFONO_DNS_CLIENT_ERROR_FORM_ERR;
	case ARES_ESERVFAIL:
		return OFONO_DNS_CLIENT_ERROR_SERVER_FAIL;
	case ARES_ENOTFOUND:
		return OFONO_DNS_CLIENT_ERROR_NOT_FOUND;
	case ARES_ENOTIMP:
		return OFONO_DNS_CLIENT_ERROR_NOT_IMP;
	case ARES_EREFUSED:
		return OFONO_DNS_CLIENT_ERROR_REFUSED;
	case ARES_EBADQUERY:
	case ARES_EBADNAME:
	case ARES_EBADFAMILY:
	case ARES_EBADRESP:
		return OFONO_DNS_CLIENT_ERROR_BAD_QUERY;
	case ARES_ECONNREFUSED:
		return OFONO_DNS_CLIENT_ERROR_NET_REFUSED;
	case ARES_ETIMEOUT:
		return OFONO_DNS_CLIENT_ERROR_TIMED_OUT;
	default:
		return OFONO_DNS_CLIENT_ERROR_UNKNOWN;
	}
}

/*
 * Callback that is invoked by c-ares when an asynchronous name resolution
 * request that we have previously initiated is complete.
 */
static void ares_request_cb(void *arg, int ares_status, int timeouts,
			    struct hostent *hostent)
{
	struct sockaddr_in sin;
	struct sockaddr_in6 sin6;
	int addr_length;
	void *addr_buffer;
	char ip_addr_string[INET6_ADDRSTRLEN];
	struct sockaddr *ip_addr;
	struct ares_request *request = (struct ares_request *)arg;

	DBG("");

	if (!request->running)
		return;

	/* Stop the request. It will be deleted later from the idle loop. */
	stop_ares_request(request);

	if (ares_status != ARES_SUCCESS) {
		DBG("ares request for '%s' failed: %s",
				request->hostname, ares_strerror(ares_status));
		/* Notify client. */
		request->cb(request->data, status_from_ares_status(ares_status),
				NULL);
		return;
	}

	if (hostent->h_addrtype != AF_INET && hostent->h_addrtype != AF_INET6) {
		ofono_error("%s: unsupported addrtype: %d",
				__func__, hostent->h_addrtype);
		request->cb(request->data, OFONO_DNS_CLIENT_ERROR_NO_DATA,
				NULL);
		return;
	}

	if (hostent->h_addrtype == AF_INET) {
		memset(&sin, 0, sizeof(sin));
		sin.sin_family = AF_INET;
		addr_length = sizeof(sin.sin_addr.s_addr);
		addr_buffer = &sin.sin_addr.s_addr;
		ip_addr = (struct sockaddr *) &sin;
	} else {  /* AF_INET6 */
		memset(&sin6, 0, sizeof(sin6));
		sin6.sin6_family = AF_INET6;
		addr_length = sizeof(sin6.sin6_addr.s6_addr);
		addr_buffer = &sin6.sin6_addr.s6_addr;
		ip_addr = (struct sockaddr *) &sin6;
	}

	if (hostent->h_length > addr_length) {
		ofono_error("%s: address too large: %u bytes",
				__func__, hostent->h_length);
		request->cb(request->data, OFONO_DNS_CLIENT_ERROR_NO_DATA,
				NULL);
		return;
	}

	memcpy(addr_buffer, hostent->h_addr, hostent->h_length);

	if (inet_ntop(hostent->h_addrtype, addr_buffer, ip_addr_string,
			sizeof(ip_addr_string)) == NULL) {
		ofono_error("%s: could not convert address to string: %s",
				__func__, strerror(errno));
		request->cb(request->data, OFONO_DNS_CLIENT_ERROR_NO_DATA,
				NULL);
		return;
	}

	DBG("ares request for '%s' succeeded with %d timeouts: %s",
			request->hostname, timeouts, ip_addr_string);
	request->cb(request->data, status_from_ares_status(ares_status),
			ip_addr);
}

/* Cancel all in-progress asynchronous name resolution requests. */
static void cancel_all_ares_requests()
{
	GList *node;
	struct ares_request *request;

	DBG("");

	while ((node = g_list_first(pending_requests)) != NULL) {
		request = node->data;
		pending_requests = g_list_delete_link(pending_requests, node);
		request->running = FALSE;  /* don't trip assertion */
		destroy_ares_request(request);
	}
}

static void set_request_servers(const char **servers,
						struct ares_request *request)
{
	struct ares_addr_node *current;
	struct ares_addr_node **prev = &request->servers;
	const char **dns;
	int rc;

	for (dns = servers; *dns != NULL; ++dns) {
		current = g_malloc0(sizeof(*current));
		rc = inet_pton(AF_INET, *dns, &current->addr.addr4);
		if (rc == 1) {
			current->family = AF_INET;
		} else {
			rc = inet_pton(AF_INET6, *dns, &current->addr.addr6);
			if (rc != 1) {
				ofono_error("%s: wrong dns address %s",
								__func__, *dns);
				g_free(current);
				continue;
			}
			current->family = AF_INET6;
		}

		*prev = current;
		prev = &current->next;
	}
}

/* Initiate an asynchronous name resolution request. */
static ofono_dns_client_request_t
ofono_dns_client_submit_request(const char *hostname,
				const char *interface,
				const char **servers,
				int timeout_ms,
				ofono_dns_client_callback_t cb,
				void *data)
{
	int ares_status;
	struct ares_request *request;
	struct ares_options options;
	int optmask;
	const gboolean destroy_old_source = TRUE;

	DBG("");

	if (timeout_ms < 0) {
		DBG("invalid timeout value of %d ms", timeout_ms);
		return NULL;
	}

	request = g_malloc0(sizeof(struct ares_request));
	request->running = TRUE;

	request->ares_watches = g_hash_table_new_full(g_int_hash, g_int_equal,
						NULL, destroy_ares_watch);
	if (request->ares_watches == NULL) {
		ofono_error("%s: could not create ares_watches table",
								__func__);
		g_free(request);
		return NULL;
	}

	/*
	 * Init a c-ares channel for this request. We set an option asking
	 * c-ares to notify us via callback about which sockets it wants to
	 * monitor for readability and writability. This allows us to
	 * integrate c-ares activity into our glib main event loop.
	 */
	memset(&options, 0, sizeof(options));
	options.sock_state_cb = ares_socket_state_cb;
	options.sock_state_cb_data = request;
	optmask = ARES_OPT_SOCK_STATE_CB;
	if (timeout_ms > 0) {
		options.timeout = timeout_ms;
		optmask |= ARES_OPT_TIMEOUTMS;
	}
	ares_status = ares_init_options(&request->channel, &options, optmask);
	if (ares_status != ARES_SUCCESS) {
		ofono_error("%s: failed to init c-ares channel: %s", __func__,
				ares_strerror(ares_status));
		request->running = FALSE;  /* don't trip assertion */
		g_hash_table_destroy(request->ares_watches);
		g_free(request);
		return NULL;
	}

	if (servers != NULL) {
		set_request_servers(servers, request);
		ares_status =
			ares_set_servers(request->channel, request->servers);
		/* Log an error, but continue anyway */
		if (ares_status != ARES_SUCCESS)
			ofono_error("%s: failed to set name servers: %s",
					__func__, ares_strerror(ares_status));
	}

	/*
	 * If the caller has provided a preferred interface, tell c-ares to
	 * send requests out that interface, and unset rf filter.
	 */
	if (interface != NULL) {
		DBG("caller has specified device %s", interface);

		request->interface = g_strdup(interface);
		ares_set_local_dev(request->channel, request->interface);
		rp_filter_set(request->interface, FALSE);
	}

	request->cb = cb;
	request->data = data;
	request->hostname = g_strdup(hostname);
	request->timeout.tv_sec = timeout_ms / 1000;
	request->timeout.tv_usec = (timeout_ms % 1000) * 1000;
	gettimeofday(&request->start_time, NULL);

	pending_requests = g_list_append(pending_requests, request);

	ares_gethostbyname(request->channel, hostname, AF_INET,
						ares_request_cb, request);

	reset_ares_timeout(request, destroy_old_source);

	return request;
}

/* Cancel an in-progress name resolution request. */
static gboolean ofono_dns_client_cancel_request(ofono_dns_client_request_t req)
{
	struct ares_request *request = req;

	if (request == NULL || g_list_find (pending_requests, request) == NULL)
		return FALSE;

	DBG("request %p", request);

	if (!request->running)
		return TRUE;

	pending_requests = g_list_remove(pending_requests, request);
	request->running = FALSE;  /* don't trip assertion */
	destroy_ares_request(request);

	return TRUE;
}

static struct ofono_dns_client_driver dns_driver = {
	.name		= "c-ares DNS client",
	.submit_request	= ofono_dns_client_submit_request,
	.cancel_request = ofono_dns_client_cancel_request
};

/* Intitialize this module. */
static int c_ares_init(void)
{
	int ares_status = 0;
	DBG("");
	ares_status = ares_library_init(ARES_LIB_INIT_ALL);
	if (ares_status != ARES_SUCCESS) {
		ofono_error("%s: Failed to init c-ares: %s", __func__,
				ares_strerror(ares_status));
		return -1;
	}
	return ofono_dns_client_driver_register(&dns_driver);
}

/* Clean up. */
static void c_ares_exit(void)
{
	DBG("");

	ofono_dns_client_driver_unregister(&dns_driver);

	if (deferred_deletion_g_source_id != 0) {
		g_source_remove(deferred_deletion_g_source_id);
		deferred_deletion_g_source_id = 0;
	}
	cancel_all_ares_requests();
	ares_library_cleanup();
}

OFONO_PLUGIN_DEFINE(c_ares_dns_client, "c-ares DNS client", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			c_ares_init, c_ares_exit)
