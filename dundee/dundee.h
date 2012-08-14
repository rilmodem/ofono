/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2012  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012  BMW Car IT GmbH. All rights reserved.
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

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE

#include <ofono/types.h>

void __dundee_exit(void);

enum dundee_error_type {
	DUNDEE_ERROR_TYPE_NO_ERROR = 0,
	DUNDEE_ERROR_TYPE_FAILURE,
};

struct dundee_error {
	enum dundee_error_type type;
	int error;
};

struct cb_data {
	void *cb;
	void *data;
	void *user;
};

static inline struct cb_data *cb_data_new(void *cb, void *data)
{
	struct cb_data *ret;

	ret = g_new0(struct cb_data, 1);
	ret->cb = cb;
	ret->data = data;

	return ret;
}

#define CALLBACK_WITH_FAILURE(cb, args...)		\
	do {						\
		struct dundee_error cb_e;		\
		cb_e.type = DUNDEE_ERROR_TYPE_FAILURE;	\
		cb_e.error = 0;				\
							\
		cb(&cb_e, ##args);			\
	} while (0)					\

#define CALLBACK_WITH_SUCCESS(f, args...)		\
	do {						\
		struct dundee_error e;			\
		e.type = DUNDEE_ERROR_TYPE_NO_ERROR;	\
		e.error = 0;				\
		f(&e, ##args);				\
	} while(0)					\

#include <ofono/log.h>

int __ofono_log_init(const char *program, const char *debug,
					ofono_bool_t detach);
void __ofono_log_cleanup(void);
void __ofono_log_enable(struct ofono_debug_desc *start,
					struct ofono_debug_desc *stop);

#include <ofono/dbus.h>

#define DUNDEE_SERVICE			"org.ofono.dundee"
#define DUNDEE_MANAGER_INTERFACE	"org.ofono.dundee.Manager"
#define DUNDEE_DEVICE_INTERFACE		"org.ofono.dundee.Device"
#define DUNDEE_MANAGER_PATH		"/"

int __ofono_dbus_init(DBusConnection *conn);
void __ofono_dbus_cleanup(void);

void __ofono_dbus_pending_reply(DBusMessage **msg, DBusMessage *reply);

DBusMessage *__dundee_error_invalid_args(DBusMessage *msg);
DBusMessage *__dundee_error_failed(DBusMessage *msg);
DBusMessage *__dundee_error_timed_out(DBusMessage *msg);


int __dundee_manager_init(void);
void __dundee_manager_cleanup(void);


struct dundee_device;

int __dundee_device_init(void);
void __dundee_device_cleanup(void);
void __dundee_device_shutdown(void);

typedef void (*dundee_device_connect_cb_t)(const struct dundee_error *error,
						int fd, void *data);
typedef void (*dundee_device_disconnect_cb_t)(const struct dundee_error *error,
						void *data);

struct dundee_device_driver {
	const char *name;

	/* Connect and dial */
	void (*connect)(struct dundee_device *device,
			dundee_device_connect_cb_t cb, void *data);

	/* Hangup and disconnect */
	void (*disconnect)(struct dundee_device *device,
			dundee_device_disconnect_cb_t cb, void *data);
};

struct dundee_device *dundee_device_create(struct dundee_device_driver *d);
int dundee_device_register(struct dundee_device *device);
void dundee_device_unregister(struct dundee_device *device);

void dundee_device_set_data(struct dundee_device *device, void *data);
void *dundee_device_get_data(struct dundee_device *device);

int dundee_device_set_name(struct dundee_device *device, const char *name);

typedef void (*dundee_device_foreach_func)(struct dundee_device *device,
						void *data);
void __dundee_device_foreach(dundee_device_foreach_func cb, void *userdata);

const char *__dundee_device_get_path(struct dundee_device *device);
void __dundee_device_append_properties(struct dundee_device *device,
					DBusMessageIter *dict);

int __dundee_bluetooth_init(void);
void  __dundee_bluetooth_cleanup(void);
