/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#include <glib.h>
#include <gdbus.h>

#include "ofono.h"

#include "common.h"

#define DATA_CONNECTION_MANAGER_INTERFACE "org.ofono.DataConnectionManager"
#define DATA_CONTEXT_INTERFACE "org.ofono.PrimaryDataContext"

#define DATA_CONNECTION_FLAG_ATTACHING 0x1

static GSList *g_drivers = NULL;

struct ofono_gprs {
	GSList *contexts;
	int attached;
	int roaming_allowed;
	int powered;
	int status;
	int location;
	int cellid;
	int technology;

	int flags;
	struct context *current_context;
	DBusMessage *pending;
	const struct ofono_gprs_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

struct context {
	struct ofono_gprs_primary_context *context;
	struct ofono_gprs *gprs;
};

static void gprs_netreg_update(struct ofono_gprs *gprs);

static gint context_compare(gconstpointer a, gconstpointer b)
{
	const struct context *ctxa = a;
	const struct context *ctxb = a;

	return ctxa->context->id - ctxb->context->id;
}

enum {
	DATA_CONTEXT_TYPE_INTERNET,
	DATA_CONTEXT_TYPE_MMS,
	DATA_CONTEXT_TYPE_WAP,
};

static inline const char *data_context_type_to_string(int type)
{
	switch (type) {
	case DATA_CONTEXT_TYPE_INTERNET:
		return "internet";
	case DATA_CONTEXT_TYPE_MMS:
		return "mms";
	case DATA_CONTEXT_TYPE_WAP:
		return "wap";
	}

	return NULL;
}

static const char *gprs_build_context_path(struct ofono_gprs *gprs,
				const struct ofono_gprs_primary_context *ctx)
{
	static char path[256];

	snprintf(path, sizeof(path), "%s/primarycontext%02u",
			__ofono_atom_get_path(gprs->atom), ctx->id);

	return path;
}

static struct context *gprs_context_by_path(struct ofono_gprs *gprs,
						const char *ctx_path)
{
	const char *path = __ofono_atom_get_path(gprs->atom);
	GSList *l;
	unsigned id;

	if (!g_str_has_prefix(ctx_path, path))
		return NULL;

	if (sscanf(ctx_path + strlen(path), "/primarycontext%2u", &id) != 1)
		return NULL;

	for (l = gprs->contexts; l; l = l->next) {
		struct context *ctx = l->data;

		if (ctx->context->id == id)
			return ctx;
	}

	return NULL;
}

static DBusMessage *pri_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct context *ctx = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t value;
	const char *type = data_context_type_to_string(ctx->context->type);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	value = ctx->context->active;
	ofono_dbus_dict_append(&dict, "Active", DBUS_TYPE_BOOLEAN, &value);

	ofono_dbus_dict_append(&dict, "AccessPointName",
				DBUS_TYPE_STRING, &ctx->context->apn);

	ofono_dbus_dict_append(&dict, "Type",
				DBUS_TYPE_STRING, &type);

	ofono_dbus_dict_append(&dict, "Username",
				DBUS_TYPE_STRING, &ctx->context->username);

	ofono_dbus_dict_append(&dict, "Passwod",
				DBUS_TYPE_STRING, &ctx->context->password);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void context_set_active_callback(const struct ofono_error *error,
					void *data)
{
	struct context *ctx = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	DBusMessage *reply;
	const char *path;
	dbus_bool_t value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Activating context failed with error: %s",
				telephony_error_to_str(error));

		reply = __ofono_error_failed(ctx->gprs->pending);
		goto reply;
	}

	reply = dbus_message_new_method_return(ctx->gprs->pending);

	if (!ctx->context->active) /* Signal emitted elsewhere */
		goto reply;

	path = gprs_build_context_path(ctx->gprs, ctx->context);
	value = ctx->context->active;
	ofono_dbus_signal_property_changed(conn, path, DATA_CONTEXT_INTERFACE,
						"Active", DBUS_TYPE_BOOLEAN,
						&value);

reply:
	__ofono_dbus_pending_reply(&ctx->gprs->pending, reply);
}

static DBusMessage *pri_set_property(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct context *ctx = data;
	DBusMessageIter iter;
	DBusMessageIter var;
	const char *property;
	dbus_bool_t value;
	const char *str;
	const char *path;

	if (ctx->gprs->pending)
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

	if (!strcmp(property, "Active")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if ((dbus_bool_t) ctx->context->active == value)
			return dbus_message_new_method_return(msg);
		if (ctx->gprs->flags & DATA_CONNECTION_FLAG_ATTACHING)
			return __ofono_error_busy(msg);
		if (value && !ctx->gprs->attached)
			return __ofono_error_failed(msg);
		if (!ctx->gprs->driver->set_active)
			return __ofono_error_not_implemented(msg);

		ctx->gprs->pending = dbus_message_ref(msg);

		ctx->gprs->driver->set_active(ctx->gprs, ctx->context->id,
						value,
						context_set_active_callback,
						ctx);

		return NULL;
	}

	/* All other properties are read-only when context is active */
	if (ctx->context->active)
		return __ofono_error_invalid_args(msg);

	if (!strcmp(property, "AccessPointName")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		if (ctx->context->apn)
			g_free(ctx->context->apn);
		ctx->context->apn = g_strdup(str);
	} else if (!strcmp(property, "Type")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		if (!strcmp(str, "internet"))
			ctx->context->type = DATA_CONTEXT_TYPE_INTERNET;
		else
			return __ofono_error_invalid_args(msg);
	} else if (!strcmp(property, "Username")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		if (ctx->context->username)
			g_free(ctx->context->username);
		ctx->context->username = g_strdup(str);
	} else if (!strcmp(property, "Password")) {
		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &str);

		if (ctx->context->password)
			g_free(ctx->context->password);
		ctx->context->password = g_strdup(str);
	} else
		return __ofono_error_invalid_args(msg);

	path = gprs_build_context_path(ctx->gprs, ctx->context);
	ofono_dbus_signal_property_changed(conn, path, DATA_CONTEXT_INTERFACE,
						property, DBUS_TYPE_STRING,
						&str);

	return dbus_message_new_method_return(msg);
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

static struct context *context_create(struct ofono_gprs *gprs,
					struct ofono_gprs_primary_context *ctx)
{
	struct context *context = g_try_new0(struct context, 1);

	if (!context)
		return NULL;

	context->context = ctx;
	context->gprs = gprs;

	return context;
}

static void context_destroy(gpointer userdata)
{
	struct context *ctx = userdata;

	g_free(ctx);
}

static gboolean context_dbus_register(struct ofono_gprs *gprs,
					struct context *ctx)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = gprs_build_context_path(gprs, ctx->context);

	if (!g_dbus_register_interface(conn, path, DATA_CONTEXT_INTERFACE,
					context_methods, context_signals,
					NULL, ctx, context_destroy)) {
		ofono_error("Could not register PrimaryContext %s", path);
		context_destroy(ctx);

		return FALSE;
	}

	return TRUE;
}

static gboolean context_dbus_unregister(struct ofono_gprs *gprs,
					struct ofono_gprs_primary_context *ctx)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = gprs_build_context_path(gprs, ctx);

	return g_dbus_unregister_interface(conn, path, DATA_CONTEXT_INTERFACE);
}

static char **gprs_contexts_path_list(struct ofono_gprs *gprs,
					GSList *context_list)
{
	GSList *l;
	char **i;
	struct context *ctx;
	char **objlist = g_new0(char *, g_slist_length(context_list) + 1);

	if (!objlist)
		return NULL;

	for (i = objlist, l = context_list; l; l = l->next) {
		ctx = l->data;
		*i++ = g_strdup(gprs_build_context_path(gprs, ctx->context));
	}

	return objlist;
}

static void gprs_generic_callback(const struct ofono_error *error, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessage *reply;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR)
		ofono_debug("command failed with error: %s",
				telephony_error_to_str(error));

	if (!gprs->pending)
		return;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR)
		reply = dbus_message_new_method_return(gprs->pending);
	else
		reply = __ofono_error_failed(gprs->pending);

	__ofono_dbus_pending_reply(&gprs->pending, reply);
}

static void gprs_attach_callback(const struct ofono_error *error, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	dbus_bool_t value;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR &&
			(gprs->flags & DATA_CONNECTION_FLAG_ATTACHING)) {
		gprs->attached = !gprs->attached;

		path = __ofono_atom_get_path(gprs->atom);
		value = gprs->attached;
		ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"Attached", DBUS_TYPE_BOOLEAN, &value);
	}

	gprs->flags &= ~DATA_CONNECTION_FLAG_ATTACHING;

	gprs_netreg_update(gprs);
}

static void gprs_netreg_update(struct ofono_gprs *gprs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	int attach;
	int operator_ok;
	const char *path;
	dbus_bool_t value = 0;

	operator_ok = gprs->roaming_allowed ||
		(gprs->status != NETWORK_REGISTRATION_STATUS_ROAMING);

	attach = gprs->powered && operator_ok;

	if (gprs->attached != attach &&
			!(gprs->flags & DATA_CONNECTION_FLAG_ATTACHING)) {
		gprs->flags |= DATA_CONNECTION_FLAG_ATTACHING;

		gprs->driver->set_attached(gprs, attach, gprs_attach_callback,
						gprs);

		/* Prevent further attempts to attach */
		if (!attach && gprs->powered) {
			gprs->powered = 0;

			path = __ofono_atom_get_path(gprs->atom);
			ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"Powered", DBUS_TYPE_BOOLEAN, &value);
		}
	}
}

static DBusMessage *gprs_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	char **objpath_list;
	dbus_bool_t value;
	const char *status = registration_status_to_string(gprs->status);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	objpath_list = gprs_contexts_path_list(gprs, gprs->contexts);
	if (!objpath_list)
		return NULL;

	ofono_dbus_dict_append_array(&dict, "PrimaryContexts",
					DBUS_TYPE_OBJECT_PATH, &objpath_list);

	g_strfreev(objpath_list);

	value = gprs->attached;
	ofono_dbus_dict_append(&dict, "Attached", DBUS_TYPE_BOOLEAN, &value);

	value = gprs->roaming_allowed;
	ofono_dbus_dict_append(&dict, "RoamingAllowed",
				DBUS_TYPE_BOOLEAN, &value);

	value = gprs->powered;
	ofono_dbus_dict_append(&dict, "Powered", DBUS_TYPE_BOOLEAN, &value);

	ofono_dbus_dict_append(&dict, "Status", DBUS_TYPE_STRING, &status);

	if (gprs->location != -1) {
		dbus_uint16_t location = gprs->location;
		ofono_dbus_dict_append(&dict, "LocationAreaCode",
					DBUS_TYPE_UINT16, &location);
	}

	if (gprs->cellid != -1) {
		dbus_uint32_t cellid = gprs->cellid;
		ofono_dbus_dict_append(&dict, "CellId",
					DBUS_TYPE_UINT32, &cellid);
	}

	if (gprs->technology != -1) {
		const char *technology =
			registration_tech_to_string(gprs->technology);

		ofono_dbus_dict_append(&dict, "Technology", DBUS_TYPE_STRING,
					&technology);
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

		gprs->roaming_allowed = value;
		gprs_netreg_update(gprs);
	} else if (!strcmp(property, "Powered")) {
		if (!gprs->driver->set_attached)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		gprs->powered = value;
		gprs_netreg_update(gprs);
	} else
		return __ofono_error_invalid_args(msg);

	path = __ofono_atom_get_path(gprs->atom);
	ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					property, DBUS_TYPE_BOOLEAN, &value);

	return dbus_message_new_method_return(msg);
}

static void gprs_create_context_callback(const struct ofono_error *error,
					struct ofono_gprs_primary_context *ctx,
					void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessage *reply;
	const char *path;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Creating new context failed with error: %s",
				telephony_error_to_str(error));

		reply = __ofono_error_failed(gprs->pending);
		goto error;
	}

	reply = dbus_message_new_method_return(gprs->pending);

	path = gprs_build_context_path(gprs, ctx);
	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);

error:
	__ofono_dbus_pending_reply(&gprs->pending, reply);
}

static DBusMessage *gprs_create_context(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!gprs->driver->create_context)
		return __ofono_error_not_implemented(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	gprs->pending = dbus_message_ref(msg);

	gprs->driver->create_context(gprs, gprs_create_context_callback, gprs);

	return NULL;
}

static void gprs_remove_context_callback(const struct ofono_error *error,
						void *data)
{
	struct ofono_gprs *gprs = data;
	DBusMessage *reply;
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	char **objpath_list;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_error("Removing context failed with error: %s",
				telephony_error_to_str(error));

		reply = __ofono_error_failed(gprs->pending);
		goto error;
	}

	context_dbus_unregister(gprs, gprs->current_context->context);
	gprs->contexts = g_slist_remove(gprs->contexts, gprs->current_context);
	gprs->current_context = NULL;

	objpath_list = gprs_contexts_path_list(gprs, gprs->contexts);
	if (!objpath_list) {
		ofono_error("Could not allocate PrimaryContext objects list");
		return;
	}

	path = __ofono_atom_get_path(gprs->atom);
	ofono_dbus_signal_array_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"PrimaryContexts",
					DBUS_TYPE_OBJECT_PATH, &objpath_list);

	g_strfreev(objpath_list);

	reply = dbus_message_new_method_return(gprs->pending);

error:
	__ofono_dbus_pending_reply(&gprs->pending, reply);
}

static void gprs_deactivate_context_callback(const struct ofono_error *error,
						void *data)
{
	struct ofono_gprs *gprs = data;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Removing context failed with error: %s",
				telephony_error_to_str(error));

		gprs->current_context = NULL;
		__ofono_dbus_pending_reply(&gprs->pending, __ofono_error_failed(
						gprs->pending));
		return;
	}

	gprs->driver->remove_context(gprs, gprs->current_context->context->id,
					gprs_remove_context_callback, gprs);
}

static DBusMessage *gprs_remove_context(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	struct context *ctx;
	const char *path;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!gprs->driver->remove_context)
		return __ofono_error_not_implemented(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (path[0] == '\0')
		return __ofono_error_invalid_format(msg);

	ctx = gprs_context_by_path(gprs, path);
	if (!ctx)
		return __ofono_error_not_found(msg);

	gprs->pending = dbus_message_ref(msg);
	gprs->current_context = ctx;

	if (ctx->context->active && gprs->driver->set_active) {
		gprs->driver->set_active(gprs, ctx->context->id, 0,
					gprs_deactivate_context_callback, gprs);

		return NULL;
	}

	gprs->driver->remove_context(gprs, ctx->context->id,
					gprs_remove_context_callback, gprs);

	return NULL;
}

static DBusMessage *gprs_deactivate_all(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!gprs->driver->set_active_all)
		return __ofono_error_not_implemented(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	gprs->pending = dbus_message_ref(msg);

	gprs->driver->set_active_all(gprs, 0, gprs_generic_callback, gprs);

	return NULL;
}

static GDBusMethodTable manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	gprs_get_properties },
	{ "SetProperty",	"sv",	"",		gprs_set_property },
	{ "CreateContext",	"",	"o",		gprs_create_context,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "RemoveContext",	"o",	"",		gprs_remove_context,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ "DeactivateAll",	"",	"",		gprs_deactivate_all,
							G_DBUS_METHOD_FLAG_ASYNC },
	{ }
};

static GDBusSignalTable manager_signals[] = {
	{ "PropertyChanged",	"sv" },
	{ }
};

void ofono_gprs_notify(struct ofono_gprs *gprs,
					struct ofono_gprs_primary_context *ctx)
{
	struct context *context = context_create(gprs, ctx);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	char **objpath_list;

	if (!context) {
		ofono_error("Unable to allocate context struct");
		return;
	}

	ofono_debug("Registering new context: %i", ctx->id);
	if (!context_dbus_register(gprs, context))
		return;

	gprs->contexts = g_slist_insert_sorted(gprs->contexts,
						context, context_compare);

	objpath_list = gprs_contexts_path_list(gprs, gprs->contexts);
	if (!objpath_list) {
		ofono_error("Unable to allocate PrimaryContext objects list");
		return;
	}

	path = __ofono_atom_get_path(gprs->atom);
	ofono_dbus_signal_array_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"PrimaryContexts",
					DBUS_TYPE_OBJECT_PATH, &objpath_list);

	g_strfreev(objpath_list);
}

void ofono_gprs_deactivated(struct ofono_gprs *gprs,
					unsigned id)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = NULL; /* Suppress warning */
	dbus_bool_t value = 0;
	GSList *l;
	struct context *ctx;

	for (l = gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->context->id == id) {
			path = gprs_build_context_path(gprs, ctx->context);
			break;
		}
	}

	ofono_dbus_signal_property_changed(conn, path, DATA_CONTEXT_INTERFACE,
						"Active", DBUS_TYPE_BOOLEAN,
						&value);
}

void ofono_gprs_detached(struct ofono_gprs *gprs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path;
	dbus_bool_t value = 0;

	if (gprs->attached && !(gprs->flags & DATA_CONNECTION_FLAG_ATTACHING)) {
		gprs->attached = 0;

		path = __ofono_atom_get_path(gprs->atom);
		ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"Attached", DBUS_TYPE_BOOLEAN, &value);

		gprs_netreg_update(gprs);
	}
}

static void set_registration_status(struct ofono_gprs *gprs,
					int status)
{
	const char *str_status = registration_status_to_string(status);
	const char *path = __ofono_atom_get_path(gprs->atom);
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t attached;

	gprs->status = status;

	ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"Status", DBUS_TYPE_STRING,
					&str_status);

	attached = (status != NETWORK_REGISTRATION_STATUS_REGISTERED &&
			status != NETWORK_REGISTRATION_STATUS_ROAMING);
	if (gprs->attached != (int) attached &&
			!(gprs->flags & DATA_CONNECTION_FLAG_ATTACHING)) {
		gprs->attached = (int) attached;

		ofono_dbus_signal_property_changed(conn, path,
				DATA_CONNECTION_MANAGER_INTERFACE,
				"Attached", DBUS_TYPE_BOOLEAN,
				&attached);

		gprs_netreg_update(gprs);
	}
}

static void set_registration_location(struct ofono_gprs *gprs,
					int lac)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(gprs->atom);
	dbus_uint16_t dbus_lac = lac;

	if (lac > 0xffff)
		return;

	gprs->location = lac;

	if (gprs->location == -1)
		return;

	ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"LocationAreaCode",
					DBUS_TYPE_UINT16, &dbus_lac);
}

static void set_registration_cellid(struct ofono_gprs *gprs, int ci)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(gprs->atom);
	dbus_uint32_t dbus_ci = ci;

	gprs->cellid = ci;

	if (gprs->cellid == -1)
		return;

	ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"CellId", DBUS_TYPE_UINT32,
					&dbus_ci);
}

static void set_registration_technology(struct ofono_gprs *gprs,
					int tech)
{
	const char *tech_str = registration_tech_to_string(tech);
	DBusConnection *conn = ofono_dbus_get_connection();
	const char *path = __ofono_atom_get_path(gprs->atom);

	gprs->technology = tech;

	if (gprs->technology == -1)
		return;

	ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"Technology", DBUS_TYPE_STRING,
					&tech_str);
}

void ofono_gprs_status_notify(struct ofono_gprs *gprs,
				int status, int lac, int ci, int tech)
{
	if (gprs->status != status)
		set_registration_status(gprs, status);

	if (gprs->location != lac)
		set_registration_location(gprs, lac);

	if (gprs->cellid != ci)
		set_registration_cellid(gprs, ci);

	if (gprs->technology != tech)
		set_registration_technology(gprs, tech);
}

int ofono_gprs_driver_register(
		const struct ofono_gprs_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_drivers = g_slist_prepend(g_drivers, (void *)d);

	return 0;
}

void ofono_gprs_driver_unregister(
		const struct ofono_gprs_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_drivers = g_slist_remove(g_drivers, (void *)d);
}

static void data_connection_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_gprs *gprs = __ofono_atom_get_data(atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);

	g_slist_free(gprs->contexts);

	ofono_modem_remove_interface(modem, DATA_CONNECTION_MANAGER_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE);
}

static void data_connection_remove(struct ofono_atom *atom)
{
	struct ofono_gprs *gprs = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (gprs == NULL)
		return;

	if (gprs->driver && gprs->driver->remove)
		gprs->driver->remove(gprs);

	g_free(gprs);
}

struct ofono_gprs *ofono_gprs_create(
		struct ofono_modem *modem, unsigned int vendor,
		const char *driver, void *data)
{
	struct ofono_gprs *gprs;
	GSList *l;

	if (driver == NULL)
		return NULL;

	gprs = g_try_new0(struct ofono_gprs, 1);

	if (gprs == NULL)
		return NULL;

	gprs->atom = __ofono_modem_add_atom(modem,
			OFONO_ATOM_TYPE_DATA_CONNECTION,
			data_connection_remove, gprs);

	for (l = g_drivers; l; l = l->next) {
		const struct ofono_gprs_driver *drv = l->data;

		if (g_strcmp0(drv->name, driver))
			continue;

		if (drv->probe(gprs, vendor, data) < 0)
			continue;

		gprs->driver = drv;
		break;
	}

	gprs->technology = -1;
	gprs->cellid = -1;
	gprs->location = -1;

	return gprs;
}

void ofono_gprs_register(struct ofono_gprs *gprs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(gprs->atom);
	const char *path = __ofono_atom_get_path(gprs->atom);

	if (!g_dbus_register_interface(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					manager_methods, manager_signals, NULL,
					gprs, NULL)) {
		ofono_error("Could not create %s interface",
				DATA_CONNECTION_MANAGER_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, DATA_CONNECTION_MANAGER_INTERFACE);

	__ofono_atom_register(gprs->atom, data_connection_unregister);
}

void ofono_gprs_remove(struct ofono_gprs *gprs)
{
	__ofono_atom_free(gprs->atom);
}

void ofono_gprs_set_data(struct ofono_gprs *gprs,
					void *data)
{
	gprs->driver_data = data;
}

void *ofono_gprs_get_data(struct ofono_gprs *gprs)
{
	return gprs->driver_data;
}
