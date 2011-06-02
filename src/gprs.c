/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2010  Intel Corporation. All rights reserved.
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"
#include "storage.h"
#include "idmap.h"
#include "simutil.h"
#include "util.h"

#define GPRS_FLAG_ATTACHING 0x1
#define GPRS_FLAG_RECHECK 0x2

#define SETTINGS_STORE "gprs"
#define SETTINGS_GROUP "Settings"
#define MAX_CONTEXT_NAME_LENGTH 127
#define MAX_MESSAGE_PROXY_LENGTH 255
#define MAX_MESSAGE_CENTER_LENGTH 255
#define MAX_CONTEXTS 256
#define SUSPEND_TIMEOUT 8

static GSList *g_drivers = NULL;
static GSList *g_context_drivers = NULL;

/* 27.007 Section 7.29 */
enum packet_bearer {
	PACKET_BEARER_NONE =		0,
	PACKET_BEARER_GPRS =		1,
	PACKET_BEARER_EGPRS =		2,
	PACKET_BEARER_UMTS =		3,
	PACKET_BEARER_HSUPA =		4,
	PACKET_BEARER_HSDPA =		5,
	PACKET_BEARER_HSUPA_HSDPA =	6,
	PACKET_BEARER_EPS =		7,
};

struct ofono_gprs {
	GSList *contexts;
	ofono_bool_t attached;
	ofono_bool_t driver_attached;
	ofono_bool_t roaming_allowed;
	ofono_bool_t powered;
	ofono_bool_t suspended;
	int status;
	int flags;
	int bearer;
	guint suspend_timeout;
	struct idmap *pid_map;
	unsigned int last_context_id;
	struct idmap *cid_map;
	int netreg_status;
	struct ofono_netreg *netreg;
	unsigned int netreg_watch;
	unsigned int status_watch;
	GKeyFile *settings;
	char *imsi;
	DBusMessage *pending;
	GSList *context_drivers;
	const struct ofono_gprs_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
	struct ofono_sim_context *sim_context;
};

struct ipv4_settings {
	gboolean static_ip;
	char *ip;
	char *netmask;
	char *gateway;
	char **dns;
	char *proxy;
};

struct ipv6_settings {
	char *ip;
	unsigned char prefix_len;
	char *gateway;
	char **dns;
};

struct context_settings {
	char *interface;
	struct ipv4_settings *ipv4;
	struct ipv6_settings *ipv6;
};

struct ofono_gprs_context {
	struct ofono_gprs *gprs;
	enum ofono_gprs_context_type type;
	ofono_bool_t inuse;
	const struct ofono_gprs_context_driver *driver;
	void *driver_data;
	struct context_settings *settings;
	struct ofono_atom *atom;
};

struct pri_context {
	ofono_bool_t active;
	enum ofono_gprs_context_type type;
	char name[MAX_CONTEXT_NAME_LENGTH + 1];
	char message_proxy[MAX_MESSAGE_PROXY_LENGTH + 1];
	char message_center[MAX_MESSAGE_CENTER_LENGTH + 1];
	unsigned int id;
	char *path;
	char *key;
	char *proxy_host;
	uint16_t proxy_port;
	DBusMessage *pending;
	struct ofono_gprs_primary_context context;
	struct ofono_gprs_context *context_driver;
	struct ofono_gprs *gprs;
};

static void gprs_netreg_update(struct ofono_gprs *gprs);
static void gprs_deactivate_next(struct ofono_gprs *gprs);

const char *packet_bearer_to_string(int bearer)
{
	switch (bearer) {
	case PACKET_BEARER_NONE:
		return "none";
	case PACKET_BEARER_GPRS:
		return "gprs";
	case PACKET_BEARER_EGPRS:
		return "edge";
	case PACKET_BEARER_UMTS:
		return "umts";
	case PACKET_BEARER_HSUPA:
		return "hsupa";
	case PACKET_BEARER_HSDPA:
		return "hsdpa";
	case PACKET_BEARER_HSUPA_HSDPA:
		return "hspa";
	case PACKET_BEARER_EPS:
		return "lte";
	}
	return "";
}

static const char *gprs_context_default_name(enum ofono_gprs_context_type type)
{
	switch (type) {
	case OFONO_GPRS_CONTEXT_TYPE_ANY:
		return NULL;
	case OFONO_GPRS_CONTEXT_TYPE_INTERNET:
		return "Internet";
	case OFONO_GPRS_CONTEXT_TYPE_MMS:
		return "MMS";
	case OFONO_GPRS_CONTEXT_TYPE_WAP:
		return "WAP";
	case OFONO_GPRS_CONTEXT_TYPE_IMS:
		return "IMS";
	}

	return NULL;
}

static const char *gprs_context_type_to_string(
					enum ofono_gprs_context_type type)
{
	switch (type) {
	case OFONO_GPRS_CONTEXT_TYPE_ANY:
		return NULL;
	case OFONO_GPRS_CONTEXT_TYPE_INTERNET:
		return "internet";
	case OFONO_GPRS_CONTEXT_TYPE_MMS:
		return "mms";
	case OFONO_GPRS_CONTEXT_TYPE_WAP:
		return "wap";
	case OFONO_GPRS_CONTEXT_TYPE_IMS:
		return "ims";
	}

	return NULL;
}

static gboolean gprs_context_string_to_type(const char *str,
					enum ofono_gprs_context_type *out)
{
	if (g_str_equal(str, "internet")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_INTERNET;
		return TRUE;
	} else if (g_str_equal(str, "wap")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_WAP;
		return TRUE;
	} else if (g_str_equal(str, "mms")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_MMS;
		return TRUE;
	} else if (g_str_equal(str, "ims")) {
		*out = OFONO_GPRS_CONTEXT_TYPE_IMS;
		return TRUE;
	}

	return FALSE;
}

static const char *gprs_proto_to_string(enum ofono_gprs_proto proto)
{
	switch (proto) {
	case OFONO_GPRS_PROTO_IP:
		return "ip";
	case OFONO_GPRS_PROTO_IPV6:
		return "ipv6";
	case OFONO_GPRS_PROTO_IPV4V6:
		return "dual";
	};

	return NULL;
}

static gboolean gprs_proto_from_string(const char *str,
					enum ofono_gprs_proto *proto)
{
	if (g_str_equal(str, "ip")) {
		*proto = OFONO_GPRS_PROTO_IP;
		return TRUE;
	} else if (g_str_equal(str, "ipv6")) {
		*proto = OFONO_GPRS_PROTO_IPV6;
		return TRUE;
	} else if (g_str_equal(str, "dual")) {
		*proto = OFONO_GPRS_PROTO_IPV4V6;
		return TRUE;
	}

	return FALSE;
}

static unsigned int gprs_cid_alloc(struct ofono_gprs *gprs)
{
	return idmap_alloc(gprs->cid_map);
}

static void gprs_cid_release(struct ofono_gprs *gprs, unsigned int id)
{
	idmap_put(gprs->cid_map, id);
}

static gboolean assign_context(struct pri_context *ctx)
{
	struct idmap *cidmap = ctx->gprs->cid_map;
	GSList *l;

	if (cidmap == NULL)
		return FALSE;

	ctx->context.cid = gprs_cid_alloc(ctx->gprs);
	if (ctx->context.cid == 0)
		return FALSE;

	for (l = ctx->gprs->context_drivers; l; l = l->next) {
		struct ofono_gprs_context *gc = l->data;

		if (gc->inuse == TRUE)
			continue;

		if (gc->driver == NULL)
			continue;

		if (gc->driver->activate_primary == NULL ||
				gc->driver->deactivate_primary == NULL)
			continue;

		if (gc->type != OFONO_GPRS_CONTEXT_TYPE_ANY &&
				gc->type != ctx->type)
			continue;

		ctx->context_driver = gc;
		ctx->context_driver->inuse = TRUE;

		if (ctx->context.proto == OFONO_GPRS_PROTO_IPV4V6 ||
				ctx->context.proto == OFONO_GPRS_PROTO_IP)
			gc->settings->ipv4 = g_new0(struct ipv4_settings, 1);

		if (ctx->context.proto == OFONO_GPRS_PROTO_IPV4V6 ||
				ctx->context.proto == OFONO_GPRS_PROTO_IPV6)
			gc->settings->ipv6 = g_new0(struct ipv6_settings, 1);

		return TRUE;
	}

	return FALSE;
}

static void release_context(struct pri_context *ctx)
{
	if (ctx == NULL || ctx->gprs == NULL || ctx->context_driver == NULL)
		return;

	gprs_cid_release(ctx->gprs, ctx->context.cid);
	ctx->context.cid = 0;
	ctx->context_driver->inuse = FALSE;
	ctx->context_driver = NULL;
	ctx->active = FALSE;
}

static struct pri_context *gprs_context_by_path(struct ofono_gprs *gprs,
						const char *ctx_path)
{
	GSList *l;

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *ctx = l->data;

		if (g_str_equal(ctx_path, ctx->path))
			return ctx;
	}

	return NULL;
}

static void context_settings_free(struct context_settings *settings)
{
	if (settings->ipv4) {
		g_free(settings->ipv4->ip);
		g_free(settings->ipv4->netmask);
		g_free(settings->ipv4->gateway);
		g_strfreev(settings->ipv4->dns);
		g_free(settings->ipv4->proxy);

		g_free(settings->ipv4);
		settings->ipv4 = NULL;
	}

	if (settings->ipv6) {
		g_free(settings->ipv6->ip);
		g_free(settings->ipv6->gateway);
		g_strfreev(settings->ipv6->dns);

		g_free(settings->ipv6);
		settings->ipv6 = NULL;
	}

	g_free(settings->interface);
	settings->interface = NULL;
}

static void context_settings_append_ipv4(struct context_settings *settings,
						DBusMessageIter *iter)
{
	DBusMessageIter variant;
	DBusMessageIter array;
	char typesig[5];
	char arraysig[6];
	const char *method;

	arraysig[0] = DBUS_TYPE_ARRAY;
	arraysig[1] = typesig[0] = DBUS_DICT_ENTRY_BEGIN_CHAR;
	arraysig[2] = typesig[1] = DBUS_TYPE_STRING;
	arraysig[3] = typesig[2] = DBUS_TYPE_VARIANT;
	arraysig[4] = typesig[3] = DBUS_DICT_ENTRY_END_CHAR;
	arraysig[5] = typesig[4] = '\0';

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						arraysig, &variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
						typesig, &array);
	if (settings == NULL || settings->ipv4 == NULL)
		goto done;

	ofono_dbus_dict_append(&array, "Interface",
				DBUS_TYPE_STRING, &settings->interface);

	/* If we have a Proxy, no other settings are relevant */
	if (settings->ipv4->proxy) {
		ofono_dbus_dict_append(&array, "Proxy", DBUS_TYPE_STRING,
					&settings->ipv4->proxy);
		goto done;
	}

	if (settings->ipv4->static_ip == TRUE)
		method = "static";
	else
		method = "dhcp";

	ofono_dbus_dict_append(&array, "Method", DBUS_TYPE_STRING, &method);

	if (settings->ipv4->ip)
		ofono_dbus_dict_append(&array, "Address", DBUS_TYPE_STRING,
					&settings->ipv4->ip);

	if (settings->ipv4->netmask)
		ofono_dbus_dict_append(&array, "Netmask", DBUS_TYPE_STRING,
					&settings->ipv4->netmask);

	if (settings->ipv4->gateway)
		ofono_dbus_dict_append(&array, "Gateway", DBUS_TYPE_STRING,
					&settings->ipv4->gateway);

	if (settings->ipv4->dns)
		ofono_dbus_dict_append_array(&array, "DomainNameServers",
						DBUS_TYPE_STRING,
						&settings->ipv4->dns);

done:
	dbus_message_iter_close_container(&variant, &array);

	dbus_message_iter_close_container(iter, &variant);
}

static void context_settings_append_ipv4_dict(struct context_settings *settings,
						DBusMessageIter *dict)
{
	DBusMessageIter entry;
	const char *key = "Settings";

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	context_settings_append_ipv4(settings, &entry);

	dbus_message_iter_close_container(dict, &entry);
}

static void context_settings_append_ipv6(struct context_settings *settings,
						DBusMessageIter *iter)
{
	DBusMessageIter variant;
	DBusMessageIter array;
	char typesig[5];
	char arraysig[6];

	arraysig[0] = DBUS_TYPE_ARRAY;
	arraysig[1] = typesig[0] = DBUS_DICT_ENTRY_BEGIN_CHAR;
	arraysig[2] = typesig[1] = DBUS_TYPE_STRING;
	arraysig[3] = typesig[2] = DBUS_TYPE_VARIANT;
	arraysig[4] = typesig[3] = DBUS_DICT_ENTRY_END_CHAR;
	arraysig[5] = typesig[4] = '\0';

	dbus_message_iter_open_container(iter, DBUS_TYPE_VARIANT,
						arraysig, &variant);

	dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
						typesig, &array);
	if (settings == NULL || settings->ipv6 == NULL)
		goto done;

	ofono_dbus_dict_append(&array, "Interface",
				DBUS_TYPE_STRING, &settings->interface);

	if (settings->ipv6->ip)
		ofono_dbus_dict_append(&array, "Address", DBUS_TYPE_STRING,
					&settings->ipv6->ip);

	if (settings->ipv6->prefix_len)
		ofono_dbus_dict_append(&array, "PrefixLength", DBUS_TYPE_BYTE,
					&settings->ipv6->prefix_len);

	if (settings->ipv6->gateway)
		ofono_dbus_dict_append(&array, "Gateway", DBUS_TYPE_STRING,
					&settings->ipv6->gateway);

	if (settings->ipv6->dns)
		ofono_dbus_dict_append_array(&array, "DomainNameServers",
						DBUS_TYPE_STRING,
						&settings->ipv6->dns);

done:
	dbus_message_iter_close_container(&variant, &array);

	dbus_message_iter_close_container(iter, &variant);
}

static void context_settings_append_ipv6_dict(struct context_settings *settings,
						DBusMessageIter *dict)
{
	DBusMessageIter entry;
	const char *key = "IPv6.Settings";

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
						NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);

	context_settings_append_ipv6(settings, &entry);

	dbus_message_iter_close_container(dict, &entry);
}

static void signal_settings(struct pri_context *ctx, const char *prop,
		void (*append)(struct context_settings *, DBusMessageIter *))

{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = ctx->path;
	DBusMessage *signal;
	DBusMessageIter iter;
	struct context_settings *settings;

	signal = dbus_message_new_signal(path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"PropertyChanged");

	if (signal == NULL)
		return;

	dbus_message_iter_init_append(signal, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &prop);

	if (ctx->context_driver)
		settings = ctx->context_driver->settings;
	else
		settings = NULL;

	append(settings, &iter);
	g_dbus_send_message(conn, signal);
}

static void pri_context_signal_settings(struct pri_context *ctx,
					gboolean ipv4, gboolean ipv6)
{
	if (ipv4)
		signal_settings(ctx, "Settings",
				context_settings_append_ipv4);

	if (ipv6)
		signal_settings(ctx, "IPv6.Settings",
				context_settings_append_ipv6);
}

static void pri_parse_proxy(struct pri_context *ctx, const char *proxy)
{
	char *scheme, *host, *port, *path;

	scheme = g_strdup(proxy);
	if (scheme == NULL)
		return;

	host = strstr(scheme, "://");
	if (host != NULL) {
		*host = '\0';
		host += 3;

		if (strcasecmp(scheme, "https") == 0)
			ctx->proxy_port = 443;
		else if (strcasecmp(scheme, "http") == 0)
			ctx->proxy_port = 80;
		else {
			g_free(scheme);
			return;
		}
	} else {
		host = scheme;
		ctx->proxy_port = 80;
	}

	path = strchr(host, '/');
	if (path != NULL)
		*(path++) = '\0';

	port = strrchr(host, ':');
	if (port != NULL) {
		char *end;
		int tmp = strtol(port + 1, &end, 10);

		if (*end == '\0') {
			*port = '\0';
			ctx->proxy_port = tmp;
		}
	}

	g_free(ctx->proxy_host);
	ctx->proxy_host = g_strdup(host);

	g_free(scheme);
}

static void pri_ifupdown(const char *interface, ofono_bool_t active)
{
	struct ifreq ifr;
	int sk;

	if (interface == NULL)
		return;

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface, IFNAMSIZ);

	if (ioctl(sk, SIOCGIFFLAGS, &ifr) < 0)
		goto done;

	if (active == TRUE) {
		if (ifr.ifr_flags & IFF_UP)
			goto done;
		ifr.ifr_flags |= IFF_UP;
	} else {
		if (!(ifr.ifr_flags & IFF_UP))
			goto done;
		ifr.ifr_flags &= ~IFF_UP;
	}

	if (ioctl(sk, SIOCSIFFLAGS, &ifr) < 0)
		ofono_error("Failed to change interface flags");

done:
	close(sk);
}

static void pri_set_ipv4_addr(const char *interface, const char *address)
{
	struct ifreq ifr;
	struct sockaddr_in addr;
	int sk;

	if (interface == NULL)
		return;

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, interface, IFNAMSIZ);

	if (ioctl(sk, SIOCGIFFLAGS, &ifr) < 0)
		goto done;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = address ? inet_addr(address) : INADDR_ANY;
	memcpy(&ifr.ifr_addr, &addr, sizeof(ifr.ifr_addr));

	if (ioctl(sk, SIOCSIFADDR, &ifr) < 0) {
		ofono_error("Failed to set interface address");
		goto done;
	}

	if (address == NULL)
		goto done;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr("255.255.255.255");
	memcpy(&ifr.ifr_netmask, &addr, sizeof(ifr.ifr_netmask));

	if (ioctl(sk, SIOCSIFNETMASK, &ifr) < 0)
		ofono_error("Failed to set interface netmask");

done:
	close(sk);
}

static void pri_setproxy(const char *interface, const char *proxy)
{
	struct rtentry rt;
	struct sockaddr_in addr;
	int sk;

	if (interface == NULL)
		return;

	sk = socket(PF_INET, SOCK_DGRAM, 0);
	if (sk < 0)
		return;

	memset(&rt, 0, sizeof(rt));
	rt.rt_flags = RTF_UP | RTF_HOST;
	rt.rt_dev = (char *) interface;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = inet_addr(proxy);
	memcpy(&rt.rt_dst, &addr, sizeof(rt.rt_dst));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	memcpy(&rt.rt_gateway, &addr, sizeof(rt.rt_gateway));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	memcpy(&rt.rt_genmask, &addr, sizeof(rt.rt_genmask));

	if (ioctl(sk, SIOCADDRT, &rt) < 0)
		ofono_error("Failed to add proxy host route");

	close(sk);
}

static void pri_reset_context_settings(struct pri_context *ctx)
{
	struct context_settings *settings;
	char *interface;
	gboolean signal_ipv4;
	gboolean signal_ipv6;

	if (ctx->context_driver == NULL)
		return;

	settings = ctx->context_driver->settings;

	interface = settings->interface;
	settings->interface = NULL;

	signal_ipv4 = settings->ipv4 != NULL;
	signal_ipv6 = settings->ipv6 != NULL;

	context_settings_free(settings);

	pri_context_signal_settings(ctx, signal_ipv4, signal_ipv6);

	if (ctx->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		pri_set_ipv4_addr(interface, NULL);

		g_free(ctx->proxy_host);
		ctx->proxy_host = NULL;
		ctx->proxy_port = 0;
	}

	pri_ifupdown(interface, FALSE);

	g_free(interface);
}

static void pri_update_mms_context_settings(struct pri_context *ctx)
{
	struct ofono_gprs_context *gc = ctx->context_driver;
	struct context_settings *settings = gc->settings;

	if (ctx->message_proxy)
		settings->ipv4->proxy = g_strdup(ctx->message_proxy);

	pri_parse_proxy(ctx, ctx->message_proxy);

	DBG("proxy %s port %u", ctx->proxy_host, ctx->proxy_port);

	pri_set_ipv4_addr(settings->interface, settings->ipv4->ip);

	if (ctx->proxy_host)
		pri_setproxy(settings->interface, ctx->proxy_host);
}

static void append_context_properties(struct pri_context *ctx,
					DBusMessageIter *dict)
{
	const char *type = gprs_context_type_to_string(ctx->type);
	const char *proto = gprs_proto_to_string(ctx->context.proto);
	const char *name = ctx->name;
	dbus_bool_t value;
	const char *strvalue;
	struct context_settings *settings;

	ofono_dbus_dict_append(dict, "Name", DBUS_TYPE_STRING, &name);

	value = ctx->active;
	ofono_dbus_dict_append(dict, "Active", DBUS_TYPE_BOOLEAN, &value);

	ofono_dbus_dict_append(dict, "Type", DBUS_TYPE_STRING, &type);

	ofono_dbus_dict_append(dict, "Protocol", DBUS_TYPE_STRING, &proto);

	strvalue = ctx->context.apn;
	ofono_dbus_dict_append(dict, "AccessPointName", DBUS_TYPE_STRING,
				&strvalue);

	strvalue = ctx->context.username;
	ofono_dbus_dict_append(dict, "Username", DBUS_TYPE_STRING,
				&strvalue);

	strvalue = ctx->context.password;
	ofono_dbus_dict_append(dict, "Password", DBUS_TYPE_STRING,
				&strvalue);

	if (ctx->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		strvalue = ctx->message_proxy;
		ofono_dbus_dict_append(dict, "MessageProxy",
					DBUS_TYPE_STRING, &strvalue);

		strvalue = ctx->message_center;
		ofono_dbus_dict_append(dict, "MessageCenter",
					DBUS_TYPE_STRING, &strvalue);
	}

	if (ctx->context_driver)
		settings = ctx->context_driver->settings;
	else
		settings = NULL;

	context_settings_append_ipv4_dict(settings, dict);
	context_settings_append_ipv6_dict(settings, dict);
}

static DBusMessage *pri_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct pri_context *ctx = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
	append_context_properties(ctx, &dict);
	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void pri_activate_callback(const struct ofono_error *error, void *data)
{
	struct pri_context *ctx = data;
	struct ofono_gprs_context *gc = ctx->context_driver;
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t value;

	DBG("%p", ctx);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Activating context failed with error: %s",
				telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&ctx->pending,
					__ofono_error_failed(ctx->pending));
		context_settings_free(ctx->context_driver->settings);
		release_context(ctx);
		return;
	}

	ctx->active = TRUE;
	__ofono_dbus_pending_reply(&ctx->pending,
				dbus_message_new_method_return(ctx->pending));

	if (gc->settings->interface != NULL) {
		pri_ifupdown(gc->settings->interface, TRUE);

		if (ctx->type == OFONO_GPRS_CONTEXT_TYPE_MMS &&
				gc->settings->ipv4)
			pri_update_mms_context_settings(ctx);

		pri_context_signal_settings(ctx, gc->settings->ipv4 != NULL,
						gc->settings->ipv6 != NULL);
	}

	value = ctx->active;
	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
}

static void pri_deactivate_callback(const struct ofono_error *error, void *data)
{
	struct pri_context *ctx = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Deactivating context failed with error: %s",
				telephony_error_to_str(error));
		__ofono_dbus_pending_reply(&ctx->pending,
					__ofono_error_failed(ctx->pending));
		return;
	}

	__ofono_dbus_pending_reply(&ctx->pending,
				dbus_message_new_method_return(ctx->pending));

	pri_reset_context_settings(ctx);
	release_context(ctx);

	value = ctx->active;
	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
}

static DBusMessage *pri_set_apn(struct pri_context *ctx, DBusConnection *conn,
				DBusMessage *msg, const char *apn)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(apn) > OFONO_GPRS_MAX_APN_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(apn, ctx->context.apn))
		return dbus_message_new_method_return(msg);

	if (is_valid_apn(apn) == FALSE)
		return __ofono_error_invalid_format(msg);

	strcpy(ctx->context.apn, apn);

	if (settings) {
		g_key_file_set_string(settings, ctx->key,
					"AccessPointName", apn);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"AccessPointName",
					DBUS_TYPE_STRING, &apn);

	return NULL;
}

static DBusMessage *pri_set_username(struct pri_context *ctx,
					DBusConnection *conn, DBusMessage *msg,
					const char *username)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(username) > OFONO_GPRS_MAX_USERNAME_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(username, ctx->context.username))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->context.username, username);

	if (settings) {
		g_key_file_set_string(settings, ctx->key,
					"Username", username);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Username",
					DBUS_TYPE_STRING, &username);

	return NULL;
}

static DBusMessage *pri_set_password(struct pri_context *ctx,
					DBusConnection *conn, DBusMessage *msg,
					const char *password)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(password) > OFONO_GPRS_MAX_PASSWORD_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(password, ctx->context.password))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->context.password, password);

	if (settings) {
		g_key_file_set_string(settings, ctx->key,
					"Password", password);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Password",
					DBUS_TYPE_STRING, &password);

	return NULL;
}

static DBusMessage *pri_set_type(struct pri_context *ctx, DBusConnection *conn,
					DBusMessage *msg, const char *type)
{
	GKeyFile *settings = ctx->gprs->settings;
	enum ofono_gprs_context_type context_type;

	if (gprs_context_string_to_type(type, &context_type) == FALSE)
		return __ofono_error_invalid_format(msg);

	if (ctx->type == context_type)
		return dbus_message_new_method_return(msg);

	ctx->type = context_type;

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "Type", type);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Type", DBUS_TYPE_STRING, &type);

	return NULL;
}

static DBusMessage *pri_set_proto(struct pri_context *ctx,
					DBusConnection *conn,
					DBusMessage *msg, const char *str)
{
	GKeyFile *settings = ctx->gprs->settings;
	enum ofono_gprs_proto proto;

	if (gprs_proto_from_string(str, &proto) == FALSE)
		return __ofono_error_invalid_format(msg);

	if (ctx->context.proto == proto)
		return dbus_message_new_method_return(msg);

	ctx->context.proto = proto;

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "Protocol", str);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Protocol", DBUS_TYPE_STRING, &str);

	return NULL;
}

static DBusMessage *pri_set_name(struct pri_context *ctx, DBusConnection *conn,
					DBusMessage *msg, const char *name)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(name) > MAX_CONTEXT_NAME_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (ctx->name && g_str_equal(ctx->name, name))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->name, name);

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "Name", ctx->name);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Name", DBUS_TYPE_STRING, &name);

	return NULL;
}

static DBusMessage *pri_set_message_proxy(struct pri_context *ctx,
					DBusConnection *conn,
					DBusMessage *msg, const char *proxy)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(proxy) > MAX_MESSAGE_PROXY_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (ctx->message_proxy && g_str_equal(ctx->message_proxy, proxy))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->message_proxy, proxy);

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "MessageProxy",
							ctx->message_proxy);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
				OFONO_CONNECTION_CONTEXT_INTERFACE,
				"MessageProxy", DBUS_TYPE_STRING, &proxy);

	return NULL;
}

static DBusMessage *pri_set_message_center(struct pri_context *ctx,
					DBusConnection *conn,
					DBusMessage *msg, const char *center)
{
	GKeyFile *settings = ctx->gprs->settings;

	if (strlen(center) > MAX_MESSAGE_CENTER_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (ctx->message_center && g_str_equal(ctx->message_center, center))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->message_center, center);

	if (settings) {
		g_key_file_set_string(settings, ctx->key, "MessageCenter",
							ctx->message_center);
		storage_sync(ctx->gprs->imsi, SETTINGS_STORE, settings);
	}

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
				OFONO_CONNECTION_CONTEXT_INTERFACE,
				"MessageCenter", DBUS_TYPE_STRING, &center);

	return NULL;
}

static DBusMessage *pri_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct pri_context *ctx = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	dbus_bool_t value;
	const char *str;

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (g_str_equal(property, "Active")) {
		struct ofono_gprs_context *gc;

		if (ctx->gprs->pending)
			return __ofono_error_busy(msg);

		if (ctx->pending)
			return __ofono_error_busy(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (ctx->active == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		if (value && !ctx->gprs->attached)
			return __ofono_error_not_attached(msg);

		if (ctx->gprs->flags & GPRS_FLAG_ATTACHING)
			return __ofono_error_attach_in_progress(msg);

		if (value && assign_context(ctx) == FALSE)
			return __ofono_error_not_implemented(msg);

		gc = ctx->context_driver;

		ctx->pending = dbus_message_ref(msg);

		if (value)
			gc->driver->activate_primary(gc, &ctx->context,
						pri_activate_callback, ctx);
		else
			gc->driver->deactivate_primary(gc, ctx->context.cid,
						pri_deactivate_callback, ctx);

		return NULL;
	}

	/* All other properties are read-only when context is active */
	if (ctx->active == TRUE)
		return __ofono_error_in_use(msg);

	if (!strcmp(property, "AccessPointName")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_apn(ctx, conn, msg, str);
	} else if (!strcmp(property, "Type")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_type(ctx, conn, msg, str);
	} else if (!strcmp(property, "Protocol")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_proto(ctx, conn, msg, str);
	} else if (!strcmp(property, "Username")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_username(ctx, conn, msg, str);
	} else if (!strcmp(property, "Password")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_password(ctx, conn, msg, str);
	} else if (!strcmp(property, "Name")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_name(ctx, conn, msg, str);
	}

	if (ctx->type != OFONO_GPRS_CONTEXT_TYPE_MMS)
		return __ofono_error_invalid_args(msg);

	if (!strcmp(property, "MessageProxy")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_message_proxy(ctx, conn, msg, str);
	} else if (!strcmp(property, "MessageCenter")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		return pri_set_message_center(ctx, conn, msg, str);
	}

	return __ofono_error_invalid_args(msg);
}

static GDBusMethodTable context_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	pri_get_properties },
	{ "SetProperty",	"sv",	"",		pri_set_property,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable context_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

static struct pri_context *pri_context_create(struct ofono_gprs *gprs,
					const char *name,
					enum ofono_gprs_context_type type)
{
	struct pri_context *context = g_try_new0(struct pri_context, 1);

	if (context == NULL)
		return NULL;

	if (name == NULL) {
		name = gprs_context_default_name(type);
		if (name == NULL) {
			g_free(context);
			return NULL;
		}
	}

	context->gprs = gprs;
	strcpy(context->name, name);
	context->type = type;

	return context;
}

static void pri_context_destroy(gpointer userdata)
{
	struct pri_context *ctx = userdata;

	g_free(ctx->proxy_host);
	g_free(ctx->path);
	g_free(ctx);
}

static gboolean context_dbus_register(struct pri_context *ctx)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	char path[256];
	const char *basepath;

	basepath = __ofono_atom_get_path(ctx->gprs->atom);

	snprintf(path, sizeof(path), "%s/context%u", basepath, ctx->id);

	if (!g_dbus_register_interface(conn, path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					context_methods, context_signals,
					NULL, ctx, pri_context_destroy)) {
		ofono_error("Could not register PrimaryContext %s", path);
		idmap_put(ctx->gprs->pid_map, ctx->id);
		pri_context_destroy(ctx);

		return FALSE;
	}

	ctx->path = g_strdup(path);
	ctx->key = ctx->path + strlen(basepath) + 1;

	return TRUE;
}

static gboolean context_dbus_unregister(struct pri_context *ctx)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	char path[256];

	if (ctx->active == TRUE) {
		const char *interface =
			ctx->context_driver->settings->interface;

		if (ctx->type == OFONO_GPRS_CONTEXT_TYPE_MMS)
			pri_set_ipv4_addr(interface, NULL);

		pri_ifupdown(interface, FALSE);
	}

	strcpy(path, ctx->path);
	idmap_put(ctx->gprs->pid_map, ctx->id);

	return g_dbus_unregister_interface(conn, path,
					OFONO_CONNECTION_CONTEXT_INTERFACE);
}

static void update_suspended_property(struct ofono_gprs *gprs,
				ofono_bool_t suspended)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(gprs->atom);
	dbus_bool_t value = suspended;

	if (gprs->suspend_timeout) {
		g_source_remove(gprs->suspend_timeout);
		gprs->suspend_timeout = 0;
	}

	if (gprs->suspended == suspended)
		return;

	DBG("%s GPRS service %s", __ofono_atom_get_path(gprs->atom),
		suspended ? "suspended" : "resumed");

	gprs->suspended = suspended;

	if (gprs->attached)
		ofono_dbus_signal_property_changed(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					"Suspended", DBUS_TYPE_BOOLEAN, &value);
}

static gboolean suspend_timeout(gpointer data)
{
       struct ofono_gprs *gprs = data;

       gprs->suspend_timeout = 0;
       update_suspended_property(gprs, TRUE);
       return FALSE;
}

void ofono_gprs_suspend_notify(struct ofono_gprs *gprs, int cause)
{
	switch (cause) {
	case GPRS_SUSPENDED_DETACHED:
	case GPRS_SUSPENDED_CALL:
	case GPRS_SUSPENDED_NO_COVERAGE:
		update_suspended_property(gprs, TRUE);
		break;

	case GPRS_SUSPENDED_SIGNALLING:
	case GPRS_SUSPENDED_UNKNOWN_CAUSE:
		if (gprs->suspend_timeout)
			g_source_remove(gprs->suspend_timeout);
		gprs->suspend_timeout = g_timeout_add_seconds(SUSPEND_TIMEOUT,
							suspend_timeout,
							gprs);
		break;
	}
}

void ofono_gprs_resume_notify(struct ofono_gprs *gprs)
{
	update_suspended_property(gprs, FALSE);
}

static void gprs_attached_update(struct ofono_gprs *gprs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	ofono_bool_t attached;
	dbus_bool_t value;

	attached = gprs->driver_attached &&
		(gprs->status == NETWORK_REGISTRATION_STATUS_REGISTERED ||
			gprs->status == NETWORK_REGISTRATION_STATUS_ROAMING);

	if (attached == gprs->attached)
		return;

	gprs->attached = attached;

	if (gprs->attached == FALSE) {
		GSList *l;
		struct pri_context *ctx;

		for (l = gprs->contexts; l; l = l->next) {
			ctx = l->data;

			if (ctx->active == FALSE)
				continue;

			pri_reset_context_settings(ctx);
			release_context(ctx);

			value = FALSE;
			ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
		}

		gprs->bearer = -1;
	}

	path = __ofono_atom_get_path(gprs->atom);
	value = attached;
	ofono_dbus_signal_property_changed(conn, path,
				OFONO_CONNECTION_MANAGER_INTERFACE,
				"Attached", DBUS_TYPE_BOOLEAN, &value);
}

static void registration_status_cb(const struct ofono_error *error,
					int status, void *data)
{
	struct ofono_gprs *gprs = data;

	DBG("%s error %d status %d", __ofono_atom_get_path(gprs->atom),
		error->type, status);

	gprs->flags &= ~GPRS_FLAG_ATTACHING;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		ofono_gprs_status_notify(gprs, status);
	else
		gprs_attached_update(gprs);

	if (gprs->flags & GPRS_FLAG_RECHECK) {
		gprs->flags &= ~GPRS_FLAG_RECHECK;
		gprs_netreg_update(gprs);
	}
}

static void gprs_attach_callback(const struct ofono_error *error, void *data)
{
	struct ofono_gprs *gprs = data;

	DBG("%s error = %d", __ofono_atom_get_path(gprs->atom), error->type);

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		gprs->driver_attached = !gprs->driver_attached;

	if (gprs->driver->attached_status == NULL) {
		struct ofono_error error;

		error.type = OFONO_ERROR_TYPE_FAILURE;
		error.error = 0;

		registration_status_cb(&error, -1, gprs);
		return;
	}

	gprs->driver->attached_status(gprs, registration_status_cb, gprs);
}

static void gprs_netreg_removed(struct ofono_gprs *gprs)
{
	gprs->netreg = NULL;

	gprs->flags &= ~(GPRS_FLAG_RECHECK | GPRS_FLAG_ATTACHING);
	gprs->status_watch = 0;
	gprs->netreg_status = NETWORK_REGISTRATION_STATUS_NOT_REGISTERED;
	gprs->driver_attached = FALSE;

	gprs_attached_update(gprs);
}

static void gprs_netreg_update(struct ofono_gprs *gprs)
{
	ofono_bool_t attach;

	attach = gprs->netreg_status == NETWORK_REGISTRATION_STATUS_REGISTERED;

	attach = attach || (gprs->roaming_allowed &&
		gprs->netreg_status == NETWORK_REGISTRATION_STATUS_ROAMING);

	attach = attach && gprs->powered;

	if (gprs->driver_attached == attach)
		return;

	if (gprs->flags & GPRS_FLAG_ATTACHING) {
		gprs->flags |= GPRS_FLAG_RECHECK;
		return;
	}

	gprs->flags |= GPRS_FLAG_ATTACHING;

	gprs->driver->set_attached(gprs, attach, gprs_attach_callback, gprs);
	gprs->driver_attached = attach;
}

static void netreg_status_changed(int status, int lac, int ci, int tech,
					const char *mcc, const char *mnc,
					void *data)
{
	struct ofono_gprs *gprs = data;

	DBG("%d", status);

	if (gprs->netreg_status == status)
		return;

	gprs->netreg_status = status;

	gprs_netreg_update(gprs);
}

static DBusMessage *gprs_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t value;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	value = gprs->attached;
	ofono_dbus_dict_append(&dict, "Attached", DBUS_TYPE_BOOLEAN, &value);

	if (gprs->bearer != -1) {
		const char *bearer = packet_bearer_to_string(gprs->bearer);

		ofono_dbus_dict_append(&dict, "Bearer",
					DBUS_TYPE_STRING, &bearer);
	}

	value = gprs->roaming_allowed;
	ofono_dbus_dict_append(&dict, "RoamingAllowed",
				DBUS_TYPE_BOOLEAN, &value);

	value = gprs->powered;
	ofono_dbus_dict_append(&dict, "Powered", DBUS_TYPE_BOOLEAN, &value);

	if (gprs->attached) {
		value = gprs->suspended;
		ofono_dbus_dict_append(&dict, "Suspended",
				DBUS_TYPE_BOOLEAN, &value);
	}

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *gprs_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	dbus_bool_t value;
	const char *path;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_iter_init(msg, &iter))
		return __ofono_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return __ofono_error_invalid_args(msg);

	dbus_message_iter_recurse(&iter, &var);

	if (!strcmp(property, "RoamingAllowed")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (gprs->roaming_allowed == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		gprs->roaming_allowed = value;

		if (gprs->settings) {
			g_key_file_set_integer(gprs->settings, SETTINGS_GROUP,
						"RoamingAllowed",
						gprs->roaming_allowed);
			storage_sync(gprs->imsi, SETTINGS_STORE,
					gprs->settings);
		}

		gprs_netreg_update(gprs);
	} else if (!strcmp(property, "Powered")) {
		if (gprs->driver->set_attached == NULL)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (gprs->powered == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		gprs->powered = value;

		if (gprs->settings) {
			g_key_file_set_integer(gprs->settings, SETTINGS_GROUP,
						"Powered", gprs->powered);
			storage_sync(gprs->imsi, SETTINGS_STORE,
					gprs->settings);
		}

		gprs_netreg_update(gprs);
	} else {
		return __ofono_error_invalid_args(msg);
	}

	path = __ofono_atom_get_path(gprs->atom);
	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					property, DBUS_TYPE_BOOLEAN, &value);

	return dbus_message_new_method_return(msg);
}

static void write_context_settings(struct ofono_gprs *gprs,
					struct pri_context *context)
{
	g_key_file_set_string(gprs->settings, context->key,
				"Name", context->name);
	g_key_file_set_string(gprs->settings, context->key,
				"AccessPointName", context->context.apn);
	g_key_file_set_string(gprs->settings, context->key,
				"Username", context->context.username);
	g_key_file_set_string(gprs->settings, context->key,
				"Password", context->context.password);
	g_key_file_set_string(gprs->settings, context->key, "Type",
				gprs_context_type_to_string(context->type));
	g_key_file_set_string(gprs->settings, context->key, "Protocol",
				gprs_proto_to_string(context->context.proto));

	if (context->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		g_key_file_set_string(gprs->settings, context->key,
					"MessageProxy",
					context->message_proxy);
		g_key_file_set_string(gprs->settings, context->key,
					"MessageCenter",
					context->message_center);
	}
}

static struct pri_context *add_context(struct ofono_gprs *gprs,
					const char *name,
					enum ofono_gprs_context_type type)
{
	unsigned int id;
	struct pri_context *context;

	if (gprs->last_context_id)
		id = idmap_alloc_next(gprs->pid_map, gprs->last_context_id);
	else
		id = idmap_alloc(gprs->pid_map);

	if (id > idmap_get_max(gprs->pid_map))
		return NULL;

	context = pri_context_create(gprs, name, type);
	if (context == NULL) {
		idmap_put(gprs->pid_map, id);
		ofono_error("Unable to allocate context struct");
		return NULL;
	}

	context->id = id;

	DBG("Registering new context");

	if (!context_dbus_register(context)) {
		ofono_error("Unable to register primary context");
		return NULL;
	}

	gprs->last_context_id = id;

	if (gprs->settings) {
		write_context_settings(gprs, context);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	gprs->contexts = g_slist_append(gprs->contexts, context);

	return context;
}

static DBusMessage *gprs_add_context(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	struct pri_context *context;
	const char *typestr;
	const char *name;
	const char *path;
	enum ofono_gprs_context_type type;
	DBusMessage *signal;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &typestr,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (gprs_context_string_to_type(typestr, &type) == FALSE)
		return __ofono_error_invalid_format(msg);

	name = gprs_context_default_name(type);
	if (name == NULL)
		name = typestr;

	context = add_context(gprs, name, type);
	if (context == NULL)
		return __ofono_error_failed(msg);

	path = context->path;

	g_dbus_send_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);

	path = __ofono_atom_get_path(gprs->atom);
	signal = dbus_message_new_signal(path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					"ContextAdded");

	if (signal) {
		DBusMessageIter iter;
		DBusMessageIter dict;

		dbus_message_iter_init_append(signal, &iter);

		path = context->path;
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH,
						&path);

		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);
		append_context_properties(context, &dict);
		dbus_message_iter_close_container(&iter, &dict);

		g_dbus_send_message(conn, signal);
	}

	return NULL;
}

static void gprs_deactivate_for_remove(const struct ofono_error *error,
						void *data)
{
	struct pri_context *ctx = data;
	struct ofono_gprs *gprs = ctx->gprs;
	DBusConnection *conn = ofono_dbus_get_connection();
	char *path;
	const char *atompath;
	dbus_bool_t value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		DBG("Removing context failed with error: %s",
				telephony_error_to_str(error));

		__ofono_dbus_pending_reply(&gprs->pending,
					__ofono_error_failed(gprs->pending));
		return;
	}

	pri_reset_context_settings(ctx);
	release_context(ctx);

	value = FALSE;
	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);

	if (gprs->settings) {
		g_key_file_remove_group(gprs->settings, ctx->key, NULL);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	/* Make a backup copy of path for signal emission below */
	path = g_strdup(ctx->path);

	context_dbus_unregister(ctx);
	gprs->contexts = g_slist_remove(gprs->contexts, ctx);

	__ofono_dbus_pending_reply(&gprs->pending,
				dbus_message_new_method_return(gprs->pending));

	atompath = __ofono_atom_get_path(gprs->atom);
	g_dbus_emit_signal(conn, atompath, OFONO_CONNECTION_MANAGER_INTERFACE,
				"ContextRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);
	g_free(path);
}

static DBusMessage *gprs_remove_context(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	struct pri_context *ctx;
	const char *path;
	const char *atompath;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (path[0] == '\0')
		return __ofono_error_invalid_format(msg);

	ctx = gprs_context_by_path(gprs, path);
	if (ctx == NULL)
		return __ofono_error_not_found(msg);

	if (ctx->active) {
		struct ofono_gprs_context *gc = ctx->context_driver;

		/* This context is already being messed with */
		if (ctx->pending)
			return __ofono_error_busy(msg);

		gprs->pending = dbus_message_ref(msg);
		gc->driver->deactivate_primary(gc, ctx->context.cid,
					gprs_deactivate_for_remove, ctx);
		return NULL;
	}

	if (gprs->settings) {
		g_key_file_remove_group(gprs->settings, ctx->key, NULL);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	DBG("Unregistering context: %s", ctx->path);
	context_dbus_unregister(ctx);
	gprs->contexts = g_slist_remove(gprs->contexts, ctx);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	atompath = __ofono_atom_get_path(gprs->atom);
	g_dbus_emit_signal(conn, atompath, OFONO_CONNECTION_MANAGER_INTERFACE,
				"ContextRemoved", DBUS_TYPE_OBJECT_PATH, &path,
				DBUS_TYPE_INVALID);

	return NULL;
}

static void gprs_deactivate_for_all(const struct ofono_error *error,
					void *data)
{
	struct pri_context *ctx = data;
	struct ofono_gprs *gprs = ctx->gprs;
	DBusConnection *conn;
	dbus_bool_t value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		__ofono_dbus_pending_reply(&gprs->pending,
					__ofono_error_failed(gprs->pending));
		return;
	}

	pri_reset_context_settings(ctx);
	release_context(ctx);

	value = ctx->active;
	conn = ofono_dbus_get_connection();
	ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);

	gprs_deactivate_next(gprs);
}

static void gprs_deactivate_next(struct ofono_gprs *gprs)
{
	GSList *l;
	struct pri_context *ctx;
	struct ofono_gprs_context *gc;

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->active == FALSE)
			continue;

		gc = ctx->context_driver;
		gc->driver->deactivate_primary(gc, ctx->context.cid,
					gprs_deactivate_for_all, ctx);

		return;
	}

	__ofono_dbus_pending_reply(&gprs->pending,
				dbus_message_new_method_return(gprs->pending));
}

static DBusMessage *gprs_deactivate_all(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	GSList *l;
	struct pri_context *ctx;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->pending)
			return __ofono_error_busy(msg);
	}

	gprs->pending = dbus_message_ref(msg);

	gprs_deactivate_next(gprs);

	return NULL;
}

static DBusMessage *gprs_get_contexts(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter array;
	DBusMessageIter entry, dict;
	const char *path;
	GSList *l;
	struct pri_context *ctx;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					DBUS_STRUCT_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_OBJECT_PATH_AS_STRING
					DBUS_TYPE_ARRAY_AS_STRING
					DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
					DBUS_TYPE_STRING_AS_STRING
					DBUS_TYPE_VARIANT_AS_STRING
					DBUS_DICT_ENTRY_END_CHAR_AS_STRING
					DBUS_STRUCT_END_CHAR_AS_STRING,
					&array);

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		path = ctx->path;

		dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT,
							NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_OBJECT_PATH,
						&path);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

		append_context_properties(ctx, &dict);
		dbus_message_iter_close_container(&entry, &dict);
		dbus_message_iter_close_container(&array, &entry);
	}

	dbus_message_iter_close_container(&iter, &array);

	return reply;
}

static GDBusMethodTable manager_methods[] = {
	{ "GetProperties",     "",     "a{sv}",     gprs_get_properties },
	{ "SetProperty",       "sv",   "",          gprs_set_property },
	{ "AddContext",        "s",    "o",         gprs_add_context,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "RemoveContext",     "o",    "",          gprs_remove_context,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "DeactivateAll",     "",     "",          gprs_deactivate_all,
						G_DBUS_METHOD_FLAG_ASYNC },
	{ "GetContexts",       "",     "a(oa{sv})", gprs_get_contexts },
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ "ContextAdded",	"oa{sv}" },
	{ "ContextRemoved",     "o" },
	{ }
};

void ofono_gprs_detached_notify(struct ofono_gprs *gprs)
{
	DBG("%s", __ofono_atom_get_path(gprs->atom));

	gprs->driver_attached = FALSE;
	gprs_attached_update(gprs);

	/*
	 * TODO: The network forced a detach, we should wait for some time
	 * and try to re-attach.  This might also be related to a suspend
	 * event while voicecall is active.
	 */
}

void ofono_gprs_status_notify(struct ofono_gprs *gprs, int status)
{
	DBG("%s status %d", __ofono_atom_get_path(gprs->atom), status);

	gprs->status = status;

	if (status != NETWORK_REGISTRATION_STATUS_REGISTERED &&
			status != NETWORK_REGISTRATION_STATUS_ROAMING) {
		gprs_attached_update(gprs);
		return;
	}

	/* We registered without being powered */
	if (gprs->powered == FALSE)
		goto detach;

	if (gprs->roaming_allowed == FALSE &&
			status == NETWORK_REGISTRATION_STATUS_ROAMING)
		goto detach;

	gprs->driver_attached = TRUE;
	gprs_attached_update(gprs);

	return;

detach:
	gprs->flags |= GPRS_FLAG_ATTACHING;
	gprs->driver->set_attached(gprs, FALSE, gprs_attach_callback, gprs);
}

void ofono_gprs_set_cid_range(struct ofono_gprs *gprs,
				unsigned int min, unsigned int max)
{
	if (gprs == NULL)
		return;

	if (gprs->cid_map)
		idmap_free(gprs->cid_map);

	gprs->cid_map = idmap_new_from_range(min, max);
}

static void gprs_context_unregister(struct ofono_atom *atom)
{
	struct ofono_gprs_context *gc = __ofono_atom_get_data(atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	GSList *l;
	struct pri_context *ctx;
	dbus_bool_t value;

	DBG("%p, %p", gc, gc->gprs);

	if (gc->gprs == NULL)
		goto done;

	for (l = gc->gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->context_driver != gc)
			continue;

		if (ctx->pending != NULL)
			__ofono_dbus_pending_reply(&ctx->pending,
					__ofono_error_failed(ctx->pending));

		if (ctx->active == FALSE)
			break;

		pri_reset_context_settings(ctx);
		release_context(ctx);

		value = FALSE;
		ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
	}

	gc->gprs->context_drivers = g_slist_remove(gc->gprs->context_drivers,
							gc);
	gc->gprs = NULL;

done:
	if (gc->settings) {
		context_settings_free(gc->settings);
		g_free(gc->settings);
		gc->settings = NULL;
	}
}

void ofono_gprs_add_context(struct ofono_gprs *gprs,
				struct ofono_gprs_context *gc)
{
	if (gc->driver == NULL)
		return;

	gc->gprs = gprs;
	gc->settings = g_new0(struct context_settings, 1);

	gprs->context_drivers = g_slist_append(gprs->context_drivers, gc);
	__ofono_atom_register(gc->atom, gprs_context_unregister);
}

void ofono_gprs_bearer_notify(struct ofono_gprs *gprs, int bearer)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	const char *value;

	if (gprs->bearer == bearer)
		return;

	gprs->bearer = bearer;
	path = __ofono_atom_get_path(gprs->atom);
	value = packet_bearer_to_string(bearer);
	ofono_dbus_signal_property_changed(conn, path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Bearer", DBUS_TYPE_STRING, &value);
}

void ofono_gprs_context_deactivated(struct ofono_gprs_context *gc,
					unsigned int cid)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	GSList *l;
	struct pri_context *ctx;
	dbus_bool_t value;

	if (gc->gprs == NULL)
		return;

	for (l = gc->gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->context.cid != cid)
			continue;

		if (ctx->active == FALSE)
			break;

		pri_reset_context_settings(ctx);
		release_context(ctx);

		value = FALSE;
		ofono_dbus_signal_property_changed(conn, ctx->path,
					OFONO_CONNECTION_CONTEXT_INTERFACE,
					"Active", DBUS_TYPE_BOOLEAN, &value);
	}
}

int ofono_gprs_context_driver_register(const struct ofono_gprs_context_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_context_drivers = g_slist_prepend(g_context_drivers, (void *) d);

	return 0;
}

void ofono_gprs_context_driver_unregister(const struct ofono_gprs_context_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_context_drivers = g_slist_remove(g_context_drivers, (void *) d);
}

static void gprs_context_remove(struct ofono_atom *atom)
{
	struct ofono_gprs_context *gc = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (gc == NULL)
		return;

	if (gc->driver && gc->driver->remove)
		gc->driver->remove(gc);

	g_free(gc);
}

struct ofono_gprs_context *ofono_gprs_context_create(struct ofono_modem *modem,
						unsigned int vendor,
						const char *driver, void *data)
{
	struct ofono_gprs_context *gc;
	GSList *l;

	if (driver == NULL)
		return NULL;

	gc = g_try_new0(struct ofono_gprs_context, 1);
	if (gc == NULL)
		return NULL;

	gc->type = OFONO_GPRS_CONTEXT_TYPE_ANY;

	gc->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_GPRS_CONTEXT,
						gprs_context_remove, gc);

	for (l = g_context_drivers; l; l = l->next) {
		const struct ofono_gprs_context_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(gc, vendor, data) < 0)
			continue;

		gc->driver = drv;
		break;
	}

	return gc;
}

void ofono_gprs_context_remove(struct ofono_gprs_context *gc)
{
	if (gc == NULL)
		return;

	__ofono_atom_free(gc->atom);
}

void ofono_gprs_context_set_data(struct ofono_gprs_context *gc, void *data)
{
	gc->driver_data = data;
}

void *ofono_gprs_context_get_data(struct ofono_gprs_context *gc)
{
	return gc->driver_data;
}

struct ofono_modem *ofono_gprs_context_get_modem(struct ofono_gprs_context *gc)
{
	return __ofono_atom_get_modem(gc->atom);
}

void ofono_gprs_context_set_type(struct ofono_gprs_context *gc,
                                        enum ofono_gprs_context_type type)
{
	DBG("type %d", type);

	gc->type = type;
}

void ofono_gprs_context_set_interface(struct ofono_gprs_context *gc,
					const char *interface)
{
	struct context_settings *settings = gc->settings;

	g_free(settings->interface);
	settings->interface = g_strdup(interface);
}

void ofono_gprs_context_set_ipv4_address(struct ofono_gprs_context *gc,
						const char *address,
						gboolean static_ip)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv4 == NULL)
		return;

	g_free(settings->ipv4->ip);
	settings->ipv4->ip = g_strdup(address);
	settings->ipv4->static_ip = static_ip;
}

void ofono_gprs_context_set_ipv4_netmask(struct ofono_gprs_context *gc,
						const char *netmask)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv4 == NULL)
		return;

	g_free(settings->ipv4->netmask);
	settings->ipv4->netmask = g_strdup(netmask);
}

void ofono_gprs_context_set_ipv4_gateway(struct ofono_gprs_context *gc,
						const char *gateway)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv4 == NULL)
		return;

	g_free(settings->ipv4->gateway);
	settings->ipv4->gateway = g_strdup(gateway);
}

void ofono_gprs_context_set_ipv4_dns_servers(struct ofono_gprs_context *gc,
						const char **dns)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv4 == NULL)
		return;

	g_strfreev(settings->ipv4->dns);
	settings->ipv4->dns = g_strdupv((char **) dns);
}

void ofono_gprs_context_set_ipv6_address(struct ofono_gprs_context *gc,
						const char *address)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv6 == NULL)
		return;

	g_free(settings->ipv6->ip);
	settings->ipv6->ip = g_strdup(address);
}

void ofono_gprs_context_set_ipv6_prefix_length(struct ofono_gprs_context *gc,
						unsigned char length)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv6 == NULL)
		return;

	settings->ipv6->prefix_len = length;
}

void ofono_gprs_context_set_ipv6_gateway(struct ofono_gprs_context *gc,
						const char *gateway)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv6 == NULL)
		return;

	g_free(settings->ipv6->gateway);
	settings->ipv6->gateway = g_strdup(gateway);
}

void ofono_gprs_context_set_ipv6_dns_servers(struct ofono_gprs_context *gc,
						const char **dns)
{
	struct context_settings *settings = gc->settings;

	if (settings->ipv6 == NULL)
		return;

	g_strfreev(settings->ipv6->dns);
	settings->ipv6->dns = g_strdupv((char **) dns);
}

int ofono_gprs_driver_register(const struct ofono_gprs_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_gprs_driver_unregister(const struct ofono_gprs_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void free_contexts(struct ofono_gprs *gprs)
{
	GSList *l;

	if (gprs->settings) {
		storage_close(gprs->imsi, SETTINGS_STORE,
				gprs->settings, TRUE);

		g_free(gprs->imsi);
		gprs->imsi = NULL;
		gprs->settings = NULL;
	}

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *context = l->data;

		context_dbus_unregister(context);
	}

	g_slist_free(gprs->contexts);
}

static void gprs_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_gprs *gprs = __ofono_atom_get_data(atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	DBG("%p", gprs);

	free_contexts(gprs);

	if (gprs->cid_map) {
		idmap_free(gprs->cid_map);
		gprs->cid_map = NULL;
	}

	if (gprs->netreg_watch) {
		if (gprs->status_watch) {
			__ofono_netreg_remove_status_watch(gprs->netreg,
							gprs->status_watch);
			gprs->status_watch = 0;
		}

		__ofono_modem_remove_atom_watch(modem, gprs->netreg_watch);
		gprs->netreg_watch = 0;
		gprs->netreg = NULL;
	}

	ofono_modem_remove_interface(modem,
					OFONO_CONNECTION_MANAGER_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE);
}

static void gprs_remove(struct ofono_atom *atom)
{
	struct ofono_gprs *gprs = __ofono_atom_get_data(atom);
	GSList *l;

	DBG("atom: %p", atom);

	if (gprs == NULL)
		return;

	if (gprs->suspend_timeout)
		g_source_remove(gprs->suspend_timeout);

	if (gprs->pid_map) {
		idmap_free(gprs->pid_map);
		gprs->pid_map = NULL;
	}

	for (l = gprs->context_drivers; l; l = l->next) {
		struct ofono_gprs_context *gc = l->data;

		gc->gprs = NULL;
	}

	g_slist_free(gprs->context_drivers);

	if (gprs->driver && gprs->driver->remove)
		gprs->driver->remove(gprs);

	if (gprs->sim_context)
		ofono_sim_context_free(gprs->sim_context);

	g_free(gprs);
}

struct ofono_gprs *ofono_gprs_create(struct ofono_modem *modem,
					unsigned int vendor,
					const char *driver, void *data)
{
	struct ofono_gprs *gprs;
	GSList *l;

	if (driver == NULL)
		return NULL;

	gprs = g_try_new0(struct ofono_gprs, 1);
	if (gprs == NULL)
		return NULL;

	gprs->atom = __ofono_modem_add_atom(modem, OFONO_ATOM_TYPE_GPRS,
						gprs_remove, gprs);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_gprs_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(gprs, vendor, data) < 0)
			continue;

		gprs->driver = drv;
		break;
	}

	gprs->status = NETWORK_REGISTRATION_STATUS_UNKNOWN;
	gprs->netreg_status = NETWORK_REGISTRATION_STATUS_UNKNOWN;
	gprs->pid_map = idmap_new(MAX_CONTEXTS);

	return gprs;
}

static void netreg_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_gprs *gprs = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		gprs_netreg_removed(gprs);
		return;
	}

	gprs->netreg = __ofono_atom_get_data(atom);
	gprs->netreg_status = ofono_netreg_get_status(gprs->netreg);
	gprs->status_watch = __ofono_netreg_add_status_watch(gprs->netreg,
					netreg_status_changed, gprs, NULL);

	gprs_netreg_update(gprs);
}

static gboolean load_context(struct ofono_gprs *gprs, const char *group)
{
	char *name = NULL;
	char *typestr = NULL;
	char *protostr = NULL;
	char *username = NULL;
	char *password = NULL;
	char *apn = NULL;
	char *msgproxy = NULL;
	char *msgcenter = NULL;
	gboolean ret = FALSE;
	gboolean legacy = FALSE;
	struct pri_context *context;
	enum ofono_gprs_context_type type;
	enum ofono_gprs_proto proto;
	unsigned int id;

	if (sscanf(group, "context%d", &id) != 1) {
		if (sscanf(group, "primarycontext%d", &id) != 1)
			goto error;

		legacy = TRUE;
	}

	if (id < 1 || id > MAX_CONTEXTS)
		goto error;

	name = g_key_file_get_string(gprs->settings, group, "Name", NULL);
	if (name == NULL)
		goto error;

	typestr = g_key_file_get_string(gprs->settings, group, "Type", NULL);
	if (typestr == NULL)
		goto error;

	if (gprs_context_string_to_type(typestr, &type) == FALSE)
		goto error;

	protostr = g_key_file_get_string(gprs->settings, group,
							"Protocol", NULL);
	if (protostr == NULL)
		protostr = g_strdup("ip");

	if (gprs_proto_from_string(protostr, &proto) == FALSE)
		goto error;

	username = g_key_file_get_string(gprs->settings, group,
						"Username", NULL);
	if (username == NULL)
		goto error;

	if (strlen(username) > OFONO_GPRS_MAX_USERNAME_LENGTH)
		goto error;

	password = g_key_file_get_string(gprs->settings, group,
						"Password", NULL);
	if (password == NULL)
		goto error;

	if (strlen(password) > OFONO_GPRS_MAX_PASSWORD_LENGTH)
		goto error;

	apn = g_key_file_get_string(gprs->settings, group,
					"AccessPointName", NULL);
	if (apn == NULL)
		goto error;

	if (strlen(apn) > OFONO_GPRS_MAX_APN_LENGTH)
		goto error;

	if (type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		msgproxy = g_key_file_get_string(gprs->settings, group,
						"MessageProxy", NULL);

		msgcenter = g_key_file_get_string(gprs->settings, group,
						"MessageCenter", NULL);
	}

	/*
	 * Accept empty (just created) APNs, but don't allow other
	 * invalid ones
	 */
	if (apn[0] != '\0' && is_valid_apn(apn) == FALSE)
		goto error;

	context = pri_context_create(gprs, name, type);
	if (context == NULL)
		goto error;

	idmap_take(gprs->pid_map, id);
	context->id = id;
	strcpy(context->context.username, username);
	strcpy(context->context.password, password);
	strcpy(context->context.apn, apn);
	context->context.proto = proto;

	if (msgproxy != NULL)
		strcpy(context->message_proxy, msgproxy);

	if (msgcenter != NULL)
		strcpy(context->message_center, msgcenter);

	if (context_dbus_register(context) == FALSE)
		goto error;

	gprs->last_context_id = id;

	gprs->contexts = g_slist_append(gprs->contexts, context);
	ret = TRUE;

	if (legacy) {
		write_context_settings(gprs, context);
		g_key_file_remove_group(gprs->settings, group, NULL);
	}

error:
	g_free(name);
	g_free(typestr);
	g_free(protostr);
	g_free(username);
	g_free(password);
	g_free(apn);
	g_free(msgproxy);
	g_free(msgcenter);

	return ret;
}

static void gprs_load_settings(struct ofono_gprs *gprs, const char *imsi)
{
	GError *error;
	gboolean legacy = FALSE;
	char **groups;
	int i;

	gprs->settings = storage_open(imsi, SETTINGS_STORE);

	if (gprs->settings == NULL)
		return;

	gprs->imsi = g_strdup(imsi);

	error = NULL;
	gprs->powered = g_key_file_get_boolean(gprs->settings, SETTINGS_GROUP,
						"Powered", &error);

	/*
	 * If any error occurs, simply switch to defaults.
	 * Default to Powered = True
	 * and RoamingAllowed = False
	 */
	if (error) {
		g_error_free(error);
		gprs->powered = TRUE;
		g_key_file_set_boolean(gprs->settings, SETTINGS_GROUP,
					"Powered", gprs->powered);
	}

	error = NULL;
	gprs->roaming_allowed = g_key_file_get_boolean(gprs->settings,
							SETTINGS_GROUP,
							"RoamingAllowed",
							&error);

	if (error) {
		g_error_free(error);
		gprs->roaming_allowed = FALSE;
		g_key_file_set_boolean(gprs->settings, SETTINGS_GROUP,
					"RoamingAllowed",
					gprs->roaming_allowed);
	}

	groups = g_key_file_get_groups(gprs->settings, NULL);

	for (i = 0; groups[i]; i++) {
		if (g_str_equal(groups[i], SETTINGS_GROUP))
			continue;

		if (!g_str_has_prefix(groups[i], "context")) {
			if (!g_str_has_prefix(groups[i], "primarycontext"))
				goto remove;

			legacy = TRUE;
		}

		if (load_context(gprs, groups[i]) == TRUE)
			continue;

remove:
		g_key_file_remove_group(gprs->settings, groups[i], NULL);
	}

	g_strfreev(groups);

	if (legacy)
		storage_sync(imsi, SETTINGS_STORE, gprs->settings);
}

static void provision_context(const struct ofono_gprs_provision_data *ap,
				struct ofono_gprs *gprs)
{
	unsigned int id;
	struct pri_context *context = NULL;

	/* Sanity check */
	if (ap == NULL)
		return;

	if (ap->name == NULL || strlen(ap->name) > MAX_CONTEXT_NAME_LENGTH)
		return;

	if (ap->apn == NULL || strlen(ap->apn) > OFONO_GPRS_MAX_APN_LENGTH)
		return;

	if (is_valid_apn(ap->apn) == FALSE)
		return;

	if (ap->username &&
			strlen(ap->username) > OFONO_GPRS_MAX_USERNAME_LENGTH)
		return;

	if (ap->password &&
			strlen(ap->password) > OFONO_GPRS_MAX_PASSWORD_LENGTH)
		return;

	if (ap->message_proxy &&
			strlen(ap->message_proxy) > MAX_MESSAGE_PROXY_LENGTH)
		return;

	if (ap->message_center &&
			strlen(ap->message_center) > MAX_MESSAGE_CENTER_LENGTH)
		return;

	if (gprs->last_context_id)
		id = idmap_alloc_next(gprs->pid_map, gprs->last_context_id);
	else
		id = idmap_alloc(gprs->pid_map);

	if (id > idmap_get_max(gprs->pid_map))
		return;

	context = pri_context_create(gprs, ap->name, ap->type);
	if (context == NULL) {
		idmap_put(gprs->pid_map, id);
		return;
	}

	context->id = id;

	if (ap->username != NULL)
		strcpy(context->context.username, ap->username);

	if (ap->password != NULL)
		strcpy(context->context.password, ap->password);

	strcpy(context->context.apn, ap->apn);
	context->context.proto = ap->proto;

	if (ap->type == OFONO_GPRS_CONTEXT_TYPE_MMS) {
		if (ap->message_proxy != NULL)
			strcpy(context->message_proxy, ap->message_proxy);

		if (ap->message_center != NULL)
			strcpy(context->message_center, ap->message_center);
	}

	if (context_dbus_register(context) == FALSE)
		return;

	gprs->last_context_id = id;

	if (gprs->settings) {
		write_context_settings(gprs, context);
		storage_sync(gprs->imsi, SETTINGS_STORE, gprs->settings);
	}

	gprs->contexts = g_slist_append(gprs->contexts, context);
}

static void provision_contexts(struct ofono_gprs *gprs, const char *mcc,
				const char *mnc, const char *spn)
{
	struct ofono_gprs_provision_data *settings;
	int count;
	int i;

	if (__ofono_gprs_provision_get_settings(mcc, mnc, spn,
						&settings, &count) == FALSE) {
		ofono_warn("Provisioning failed");
		return;
	}

	for (i = 0; i < count; i++)
		provision_context(&settings[i], gprs);

	__ofono_gprs_provision_free_settings(settings, count);
}

static void ofono_gprs_finish_register(struct ofono_gprs *gprs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(gprs->atom);
	const char *path = __ofono_atom_get_path(gprs->atom);

	if (gprs->contexts == NULL) /* Automatic provisioning failed */
		add_context(gprs, NULL, OFONO_GPRS_CONTEXT_TYPE_INTERNET);

	if (!g_dbus_register_interface(conn, path,
					OFONO_CONNECTION_MANAGER_INTERFACE,
					manager_methods, manager_signals, NULL,
					gprs, NULL)) {
		ofono_error("Could not create %s interface",
				OFONO_CONNECTION_MANAGER_INTERFACE);

		free_contexts(gprs);
		return;
	}

	ofono_modem_add_interface(modem,
				OFONO_CONNECTION_MANAGER_INTERFACE);

	gprs->netreg_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_NETREG,
					netreg_watch, gprs, NULL);

	__ofono_atom_register(gprs->atom, gprs_unregister);
}

static void sim_spn_read_cb(int ok, int length, int record,
				const unsigned char *data,
				int record_length, void *userdata)
{
	struct ofono_gprs *gprs	= userdata;
	char *spn = NULL;
	struct ofono_atom *sim_atom;
	struct ofono_sim *sim = NULL;

	if (ok)
		spn = sim_string_to_utf8(data + 1, length - 1);

	sim_atom = __ofono_modem_find_atom(__ofono_atom_get_modem(gprs->atom),
						OFONO_ATOM_TYPE_SIM);
	if (sim_atom) {
		sim = __ofono_atom_get_data(sim_atom);
		provision_contexts(gprs, ofono_sim_get_mcc(sim),
					ofono_sim_get_mnc(sim), spn);
	}

	g_free(spn);
	ofono_gprs_finish_register(gprs);
}

void ofono_gprs_register(struct ofono_gprs *gprs)
{
	struct ofono_modem *modem = __ofono_atom_get_modem(gprs->atom);
	struct ofono_atom *sim_atom;
	struct ofono_sim *sim = NULL;

	sim_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_SIM);

	if (sim_atom) {
		const char *imsi;
		sim = __ofono_atom_get_data(sim_atom);

		imsi = ofono_sim_get_imsi(sim);
		gprs_load_settings(gprs, imsi);
	}

	if (gprs->contexts == NULL && sim != NULL) {
		/* Get Service Provider Name from SIM for provisioning */
		gprs->sim_context = ofono_sim_context_create(sim);

		if (ofono_sim_read(gprs->sim_context, SIM_EFSPN_FILEID,
				OFONO_SIM_FILE_STRUCTURE_TRANSPARENT,
					sim_spn_read_cb, gprs) >= 0)
			return;
	}

	ofono_gprs_finish_register(gprs);
}

void ofono_gprs_remove(struct ofono_gprs *gprs)
{
	__ofono_atom_free(gprs->atom);
}

void ofono_gprs_set_data(struct ofono_gprs *gprs, void *data)
{
	gprs->driver_data = data;
}

void *ofono_gprs_get_data(struct ofono_gprs *gprs)
{
	return gprs->driver_data;
}
