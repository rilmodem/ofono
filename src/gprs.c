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

#define GPRS_FLAG_ATTACHING 0x1

static GSList *g_drivers = NULL;
static GSList *g_context_drivers = NULL;

enum gprs_context_type {
	GPRS_CONTEXT_TYPE_INTERNET = 1,
	GPRS_CONTEXT_TYPE_MMS,
	GPRS_CONTEXT_TYPE_WAP,
};

struct ofono_gprs {
	GSList *contexts;
	ofono_bool_t attached;
	ofono_bool_t driver_attached;
	ofono_bool_t roaming_allowed;
	ofono_bool_t powered;
	int status;
	int location;
	int cellid;
	int technology;
	int flags;
	int next_context_id;
	int cid_min;
	int cid_max;
	int netreg_status;
	struct ofono_netreg *netreg;
	unsigned int netreg_watch;
	unsigned int status_watch;
	DBusMessage *pending;
	struct ofono_gprs_context *context_driver;
	const struct ofono_gprs_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

struct ofono_gprs_context {
	struct ofono_gprs *gprs;
	DBusMessage *pending;
	const struct ofono_gprs_context_driver *driver;
	void *driver_data;
	struct ofono_atom *atom;
};

struct pri_context {
	ofono_bool_t active;
	ofono_bool_t pending_active;
	enum gprs_context_type type;
	char *name;
	char *path;
	struct ofono_gprs_primary_context context;
	struct ofono_gprs *gprs;
};

static void gprs_netreg_update(struct ofono_gprs *gprs);

static inline const char *gprs_context_type_to_string(int type)
{
	switch (type) {
	case GPRS_CONTEXT_TYPE_INTERNET:
		return "internet";
	case GPRS_CONTEXT_TYPE_MMS:
		return "mms";
	case GPRS_CONTEXT_TYPE_WAP:
		return "wap";
	}

	return NULL;
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

static DBusMessage *pri_get_properties(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct pri_context *ctx = data;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	dbus_bool_t value;
	const char *type = gprs_context_type_to_string(ctx->type);
	const char *name = ctx->name ? ctx->name : "";
	const char *strvalue;

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					OFONO_PROPERTIES_ARRAY_SIGNATURE,
					&dict);

	ofono_dbus_dict_append(&dict, "Name", DBUS_TYPE_STRING, &name);

	value = ctx->active;
	ofono_dbus_dict_append(&dict, "Active", DBUS_TYPE_BOOLEAN, &value);

	ofono_dbus_dict_append(&dict, "Type", DBUS_TYPE_STRING, &type);

	strvalue = ctx->context.apn;
	ofono_dbus_dict_append(&dict, "AccessPointName", DBUS_TYPE_STRING,
				&strvalue);

	strvalue = ctx->context.username;
	ofono_dbus_dict_append(&dict, "Username", DBUS_TYPE_STRING,
				&strvalue);

	strvalue = ctx->context.password;
	ofono_dbus_dict_append(&dict, "Password", DBUS_TYPE_STRING,
				&strvalue);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static void pri_set_active_callback(const struct ofono_error *error,
					void *data)
{
	struct pri_context *ctx = data;
	struct ofono_gprs_context *gc = ctx->gprs->context_driver;
	DBusConnection *conn = ofono_dbus_get_connection();
	dbus_bool_t value;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("(De)Activating context failed with error: %s",
				telephony_error_to_str(error));

		ctx->pending_active = ctx->active;

		__ofono_dbus_pending_reply(&gc->pending,
					__ofono_error_failed(gc->pending));
		return;
	}

	ctx->active = ctx->pending_active;

	__ofono_dbus_pending_reply(&gc->pending,
				dbus_message_new_method_return(gc->pending));

	value = ctx->active;
	ofono_dbus_signal_property_changed(conn, ctx->path,
						DATA_CONTEXT_INTERFACE,
						"Active", DBUS_TYPE_BOOLEAN,
						&value);
}

static DBusMessage *pri_set_apn(struct pri_context *ctx, DBusConnection *conn,
				DBusMessage *msg, const char *apn)
{
	if (strlen(apn) > OFONO_GPRS_MAX_APN_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(apn, ctx->context.apn))
		return dbus_message_new_method_return(msg);

	if (is_valid_apn(apn) == FALSE)
		return __ofono_error_invalid_format(msg);

	strcpy(ctx->context.apn, apn);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
						DATA_CONTEXT_INTERFACE,
						"AccessPointName",
						DBUS_TYPE_STRING, &apn);

	return NULL;
}

static DBusMessage *pri_set_username(struct pri_context *ctx,
					DBusConnection *conn, DBusMessage *msg,
					const char *username)
{
	if (strlen(username) > OFONO_GPRS_MAX_USERNAME_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(username, ctx->context.username))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->context.username, username);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
						DATA_CONTEXT_INTERFACE,
						"Username",
						DBUS_TYPE_STRING, &username);

	return NULL;
}

static DBusMessage *pri_set_password(struct pri_context *ctx,
					DBusConnection *conn, DBusMessage *msg,
					const char *password)
{
	if (strlen(password) > OFONO_GPRS_MAX_PASSWORD_LENGTH)
		return __ofono_error_invalid_format(msg);

	if (g_str_equal(password, ctx->context.password))
		return dbus_message_new_method_return(msg);

	strcpy(ctx->context.password, password);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
						DATA_CONTEXT_INTERFACE,
						"Password",
						DBUS_TYPE_STRING, &password);

	return NULL;
}

static DBusMessage *pri_set_type(struct pri_context *ctx, DBusConnection *conn,
					DBusMessage *msg, const char *type)
{
	enum gprs_context_type context_type;

	if (g_str_equal(type, "internet"))
		context_type = GPRS_CONTEXT_TYPE_INTERNET;
	else if (g_str_equal(type, "wap"))
		context_type = GPRS_CONTEXT_TYPE_WAP;
	else if (g_str_equal(type, "mms"))
		context_type = GPRS_CONTEXT_TYPE_MMS;
	else
		return __ofono_error_invalid_args(msg);

	if (ctx->type == context_type)
		return dbus_message_new_method_return(msg);

	ctx->type = context_type;

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
						DATA_CONTEXT_INTERFACE, "Type",
						DBUS_TYPE_STRING, &type);

	return NULL;
}

static DBusMessage *pri_set_name(struct pri_context *ctx, DBusConnection *conn,
					DBusMessage *msg, const char *name)
{
	if (ctx->name && g_str_equal(ctx->name, name))
		return dbus_message_new_method_return(msg);

	if (ctx->name)
		g_free(ctx->name);

	ctx->name = g_strdup(name);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	ofono_dbus_signal_property_changed(conn, ctx->path,
						DATA_CONTEXT_INTERFACE, "Name",
						DBUS_TYPE_STRING, &name);

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
		struct ofono_gprs_context *gc = ctx->gprs->context_driver;

		if (gc->pending)
			return __ofono_error_busy(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (ctx->active == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		if (ctx->gprs->flags & GPRS_FLAG_ATTACHING)
			return __ofono_error_busy(msg);

		if (value && !ctx->gprs->attached)
			return __ofono_error_failed(msg);

		if (gc == NULL || gc->driver->activate_primary == NULL ||
				gc->driver->deactivate_primary == NULL)
			return __ofono_error_not_implemented(msg);

		gc->pending = dbus_message_ref(msg);

		ctx->pending_active = value;
		/* TODO: Find lowest unused CID */
		ctx->context.cid = 1;

		if (value)
			gc->driver->activate_primary(gc, &ctx->context,
						pri_set_active_callback, ctx);
		else
			gc->driver->deactivate_primary(gc, ctx->context.cid,
						pri_set_active_callback, ctx);

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

static struct pri_context *pri_context_create(struct ofono_gprs *gprs)
{
	struct pri_context *context = g_try_new0(struct pri_context, 1);

	if (!context)
		return NULL;

	context->gprs = gprs;

	return context;
}

static void pri_context_destroy(gpointer userdata)
{
	struct pri_context *ctx = userdata;

	if (ctx->name)
		g_free(ctx->name);

	if (ctx->path)
		g_free(ctx->path);

	g_free(ctx);
}

static gboolean context_dbus_register(struct pri_context *ctx)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	char path[256];
	unsigned int id = ctx->gprs->next_context_id;

	snprintf(path, sizeof(path), "%s/primarycontext%u",
			__ofono_atom_get_path(ctx->gprs->atom), id);

	if (!g_dbus_register_interface(conn, path, DATA_CONTEXT_INTERFACE,
					context_methods, context_signals,
					NULL, ctx, pri_context_destroy)) {
		ofono_error("Could not register PrimaryContext %s", path);
		pri_context_destroy(ctx);

		return FALSE;
	}

	ctx->path = g_strdup(path);

	ctx->gprs->next_context_id += 1;

	return TRUE;
}

static gboolean context_dbus_unregister(struct pri_context *ctx)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	char path[256];

	strcpy(path, ctx->path);

	return g_dbus_unregister_interface(conn, path,
						DATA_CONTEXT_INTERFACE);
}

static char **gprs_contexts_path_list(GSList *context_list)
{
	GSList *l;
	char **i;
	struct pri_context *ctx;
	char **objlist = g_new0(char *, g_slist_length(context_list) + 1);

	if (!objlist)
		return NULL;

	for (i = objlist, l = context_list; l; l = l->next) {
		ctx = l->data;
		*i++ = g_strdup(ctx->path);
	}

	return objlist;
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

	path = __ofono_atom_get_path(gprs->atom);
	value = attached;
	ofono_dbus_signal_property_changed(conn, path,
				DATA_CONNECTION_MANAGER_INTERFACE,
				"Attached", DBUS_TYPE_BOOLEAN, &value);
}

static void gprs_attach_callback(const struct ofono_error *error, void *data)
{
	struct ofono_gprs *gprs = data;

	gprs->flags &= ~GPRS_FLAG_ATTACHING;

	if (error->type == OFONO_ERROR_TYPE_NO_ERROR) {
		gprs->driver_attached = !gprs->driver_attached;
		gprs_attached_update(gprs);
	}
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

	if (gprs->flags & GPRS_FLAG_ATTACHING)
		return;

	gprs->flags |= GPRS_FLAG_ATTACHING;

	gprs->driver->set_attached(gprs, attach, gprs_attach_callback, gprs);
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

	objpath_list = gprs_contexts_path_list(gprs->contexts);
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

		if (gprs->roaming_allowed == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

		gprs->roaming_allowed = value;

		gprs_netreg_update(gprs);
	} else if (!strcmp(property, "Powered")) {
		if (!gprs->driver->set_attached)
			return __ofono_error_not_implemented(msg);

		if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_BOOLEAN)
			return __ofono_error_invalid_args(msg);

		dbus_message_iter_get_basic(&var, &value);

		if (gprs->powered == (ofono_bool_t) value)
			return dbus_message_new_method_return(msg);

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

static DBusMessage *gprs_create_context(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	struct pri_context *context;
	const char *path;
	char **objpath_list;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	context = pri_context_create(gprs);

	if (!context) {
		ofono_error("Unable to allocate context struct");
		return __ofono_error_failed(msg);
	}

	ofono_debug("Registering new context");

	if (!context_dbus_register(context)) {
		ofono_error("Unable to register primary context");
		return __ofono_error_failed(msg);
	}

	gprs->contexts = g_slist_append(gprs->contexts, context);

	objpath_list = gprs_contexts_path_list(gprs->contexts);

	if (objpath_list) {
		path = __ofono_atom_get_path(gprs->atom);
		ofono_dbus_signal_array_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"PrimaryContexts",
					DBUS_TYPE_OBJECT_PATH, &objpath_list);

		g_strfreev(objpath_list);
	}

	path = context->path;

	return g_dbus_create_reply(msg, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID);
}

static void gprs_deactivate_for_remove(const struct ofono_error *error,
						void *data)
{
	struct pri_context *ctx = data;
	struct ofono_gprs *gprs = ctx->gprs;
	char **objpath_list;

	if (error->type != OFONO_ERROR_TYPE_NO_ERROR) {
		ofono_debug("Removing context failed with error: %s",
				telephony_error_to_str(error));

		__ofono_dbus_pending_reply(&gprs->pending,
					__ofono_error_failed(gprs->pending));
		return;
	}

	context_dbus_unregister(ctx);
	gprs->contexts = g_slist_remove(gprs->contexts, ctx);

	__ofono_dbus_pending_reply(&gprs->pending,
				dbus_message_new_method_return(gprs->pending));

	objpath_list = gprs_contexts_path_list(gprs->contexts);

	if (objpath_list) {
		const char *path = __ofono_atom_get_path(gprs->atom);
		DBusConnection *conn = ofono_dbus_get_connection();

		ofono_dbus_signal_array_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"PrimaryContexts",
					DBUS_TYPE_OBJECT_PATH, &objpath_list);
		g_strfreev(objpath_list);
	}
}

static DBusMessage *gprs_remove_context(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;
	struct pri_context *ctx;
	const char *path;
	char **objpath_list;

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path,
					DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	if (path[0] == '\0')
		return __ofono_error_invalid_format(msg);

	ctx = gprs_context_by_path(gprs, path);
	if (!ctx)
		return __ofono_error_not_found(msg);

	if (ctx->active) {
		struct ofono_gprs_context *gc = gprs->context_driver;

		gprs->pending = dbus_message_ref(msg);
		gc->driver->deactivate_primary(gc, ctx->context.cid,
					gprs_deactivate_for_remove, ctx);
		return NULL;
	}

	ofono_debug("Unregistering context: %s\n", ctx->path);
	context_dbus_unregister(ctx);
	gprs->contexts = g_slist_remove(gprs->contexts, ctx);

	g_dbus_send_reply(conn, msg, DBUS_TYPE_INVALID);

	objpath_list = gprs_contexts_path_list(gprs->contexts);

	if (objpath_list) {
		path = __ofono_atom_get_path(gprs->atom);
		ofono_dbus_signal_array_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"PrimaryContexts",
					DBUS_TYPE_OBJECT_PATH, &objpath_list);
		g_strfreev(objpath_list);
	}

	return NULL;
}

static DBusMessage *gprs_deactivate_all(DBusConnection *conn,
					DBusMessage *msg, void *data)
{
	struct ofono_gprs *gprs = data;

	if (gprs->pending)
		return __ofono_error_busy(msg);

	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_INVALID))
		return __ofono_error_invalid_args(msg);

	return __ofono_error_not_implemented(msg);
}

static GDBusMethodTable manager_methods[] = {
	{ "GetProperties",	"",	"a{sv}",	gprs_get_properties },
	{ "SetProperty",	"sv",	"",		gprs_set_property },
	{ "CreateContext",	"",	"o",		gprs_create_context },
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

void ofono_gprs_detached_notify(struct ofono_gprs *gprs)
{
	if (gprs->driver_attached == FALSE)
		return;

	gprs->driver_attached = FALSE;

	gprs_attached_update(gprs);

	/* TODO: The network forced a detach, we should wait for some time
	 * and try to re-attach
	 */
}

static void set_registration_status(struct ofono_gprs *gprs, int status)
{
	const char *str_status = registration_status_to_string(status);
	const char *path = __ofono_atom_get_path(gprs->atom);
	DBusConnection *conn = ofono_dbus_get_connection();

	gprs->status = status;

	ofono_dbus_signal_property_changed(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					"Status", DBUS_TYPE_STRING,
					&str_status);

	gprs_attached_update(gprs);
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
	/* If we are not attached and haven't tried to attach, ignore */
	if (gprs->driver_attached == FALSE &&
			(gprs->flags & GPRS_FLAG_ATTACHING) == 0)
		return;

	if (gprs->status != status)
		set_registration_status(gprs, status);

	if (gprs->location != lac)
		set_registration_location(gprs, lac);

	if (gprs->cellid != ci)
		set_registration_cellid(gprs, ci);

	if (gprs->technology != tech)
		set_registration_technology(gprs, tech);
}

void ofono_gprs_set_cid_range(struct ofono_gprs *gprs, int min, int max)
{
	if (gprs == NULL)
		return;

	gprs->cid_min = min;
	gprs->cid_max = max;
}

static void gprs_context_unregister(struct ofono_atom *atom)
{
	struct ofono_gprs_context *gc = __ofono_atom_get_data(atom);

	if (gc->gprs)
		gc->gprs->context_driver = NULL;

	gc->gprs = NULL;
}

void ofono_gprs_add_context(struct ofono_gprs *gprs,
				struct ofono_gprs_context *gc)
{
	gprs->context_driver = gc;
	gc->gprs = gprs;

	__ofono_atom_register(gc->atom, gprs_context_unregister);
}

void ofono_gprs_context_deactivated(struct ofono_gprs_context *gc, unsigned cid)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	GSList *l;
	struct pri_context *ctx;
	dbus_bool_t value;

	for (l = gc->gprs->contexts; l; l = l->next) {
		ctx = l->data;

		if (ctx->active == FALSE)
			continue;

		if (ctx->context.cid != cid)
			continue;

		ctx->active = FALSE;

		value = FALSE;
		ofono_dbus_signal_property_changed(conn, ctx->path,
						DATA_CONTEXT_INTERFACE,
						"Active", DBUS_TYPE_BOOLEAN,
						&value);
	}
}

int ofono_gprs_context_driver_register(const struct ofono_gprs_context_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	if (d->probe == NULL)
		return -EINVAL;

	g_context_drivers = g_slist_prepend(g_context_drivers, (void *)d);

	return 0;
}

void ofono_gprs_context_driver_unregister(const struct ofono_gprs_context_driver *d)
{
	DBG("driver: %p, name: %s", d, d->name);

	g_context_drivers = g_slist_remove(g_context_drivers, (void *)d);
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

static void gprs_unregister(struct ofono_atom *atom)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_gprs *gprs = __ofono_atom_get_data(atom);
	struct ofono_modem *modem = __ofono_atom_get_modem(atom);
	const char *path = __ofono_atom_get_path(atom);
	GSList *l;

	for (l = gprs->contexts; l; l = l->next) {
		struct pri_context *context = l->data;

		context_dbus_unregister(context);
	}

	g_slist_free(gprs->contexts);

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

	ofono_modem_remove_interface(modem, DATA_CONNECTION_MANAGER_INTERFACE);
	g_dbus_unregister_interface(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE);
}

static void gprs_remove(struct ofono_atom *atom)
{
	struct ofono_gprs *gprs = __ofono_atom_get_data(atom);

	DBG("atom: %p", atom);

	if (gprs == NULL)
		return;

	if (gprs->context_driver) {
		gprs->context_driver->gprs = NULL;
		gprs->context_driver = NULL;
	}

	if (gprs->driver && gprs->driver->remove)
		gprs->driver->remove(gprs);

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
	gprs->technology = -1;
	gprs->cellid = -1;
	gprs->location = -1;
	gprs->next_context_id = 1;

	return gprs;
}

static void netreg_watch(struct ofono_atom *atom,
				enum ofono_atom_watch_condition cond,
				void *data)
{
	struct ofono_gprs *gprs = data;

	if (cond == OFONO_ATOM_WATCH_CONDITION_UNREGISTERED) {
		gprs->status_watch = 0;
		gprs->netreg = NULL;
		return;
	}

	gprs->netreg = __ofono_atom_get_data(atom);
	gprs->netreg_status = ofono_netreg_get_status(gprs->netreg);
	gprs->status_watch = __ofono_netreg_add_status_watch(gprs->netreg,
					netreg_status_changed, gprs, NULL);

	gprs_netreg_update(gprs);
}

void ofono_gprs_register(struct ofono_gprs *gprs)
{
	DBusConnection *conn = ofono_dbus_get_connection();
	struct ofono_modem *modem = __ofono_atom_get_modem(gprs->atom);
	const char *path = __ofono_atom_get_path(gprs->atom);
	struct ofono_atom *netreg_atom;

	if (!g_dbus_register_interface(conn, path,
					DATA_CONNECTION_MANAGER_INTERFACE,
					manager_methods, manager_signals, NULL,
					gprs, NULL)) {
		ofono_error("Could not create %s interface",
				DATA_CONNECTION_MANAGER_INTERFACE);

		return;
	}

	ofono_modem_add_interface(modem, DATA_CONNECTION_MANAGER_INTERFACE);

	/* TODO: Read Powered from SIM store */
	gprs->powered = TRUE;

	gprs->netreg_watch = __ofono_modem_add_atom_watch(modem,
					OFONO_ATOM_TYPE_NETREG,
					netreg_watch, gprs, NULL);

	netreg_atom = __ofono_modem_find_atom(modem, OFONO_ATOM_TYPE_NETREG);

	if (netreg_atom && __ofono_atom_get_registered(netreg_atom))
		netreg_watch(netreg_atom,
				OFONO_ATOM_WATCH_CONDITION_REGISTERED, gprs);

	__ofono_atom_register(gprs->atom, gprs_unregister);
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
