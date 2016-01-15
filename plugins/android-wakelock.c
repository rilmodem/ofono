/*
 *
 *  oFono - Open Source Telephony - Android based wakelocks
 *
 *  Copyright (C) 2016 Canonical Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
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

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <glib.h>
#include <dlfcn.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE

#include "plugin.h"
#include "log.h"
#include "wakelock.h"

#define ANDROID_WAKELOCK_LOCK_PATH		"/sys/power/wake_lock"
#define ANDROID_WAKELOCK_UNLOCK_PATH	"/sys/power/wake_unlock"

struct wakelock {
	char *name;
	unsigned int acquisitions;
};

GSList *locks = NULL;

static int file_exists(char const *filename)
{
	struct stat st;

	return stat(filename, &st) == 0;
}

static int write_file(const char *file, const char *content)
{
	int fd;
	unsigned int r = 0;

	fd = open(file, O_WRONLY);

	if (fd == -1)
		return -EIO;

	r = write(fd, content, strlen(content));

	close(fd);

	if (r != strlen(content))
		return -EIO;

	return 0;
}

static int wakelock_lock(const char *name)
{
	return write_file(ANDROID_WAKELOCK_LOCK_PATH, name);
}

static int wakelock_unlock(const char *name)
{
	return write_file(ANDROID_WAKELOCK_UNLOCK_PATH, name);
}

static int android_wakelock_acquire(struct wakelock *lock)
{
	if (!lock)
		return -EINVAL;

	if (lock->acquisitions > 0) {
		lock->acquisitions++;
		return 0;
	}

	if (wakelock_lock(lock->name) < 0)
		return -EIO;

	lock->acquisitions++;

	return 0;
}

static int android_wakelock_release(struct wakelock *lock)
{
	if (!lock)
		return -EINVAL;

	DBG("lock %p name %s acquisitions %d",
		lock, lock->name, lock->acquisitions);

	if (!lock->acquisitions) {
		ofono_warn("Attempted to release already released lock %s", lock->name);
		return -EINVAL;
	}

	if (lock->acquisitions > 1) {
		lock->acquisitions--;
		DBG("lock %s released acquisitions %d", lock->name, lock->acquisitions);
		return 0;
	}

	if (wakelock_unlock(lock->name) < 0)
		return -EIO;

	lock->acquisitions = 0;

	DBG("lock %s was released", lock->name);

	return 0;
}

static int android_wakelock_create(const char *name, struct wakelock **lock)
{
	if (!lock)
		return -EINVAL;

	*lock = g_new0(struct wakelock, 1);
	(*lock)->name = g_strdup(name);
	(*lock)->acquisitions = 0;

	locks = g_slist_prepend(locks, *lock);

	DBG("wakelock %s create", name);

	return 0;
}

static int android_wakelock_free(struct wakelock *lock)
{
	if (!lock)
		return -EINVAL;

	if (lock->acquisitions) {
		/* Need to force releasing the lock here */
		lock->acquisitions = 1;
		android_wakelock_release(lock);
	}

	locks = g_slist_remove(locks, lock);

	DBG("Freeing lock %s", lock->name);

	g_free(lock->name);
	g_free(lock);

	return 0;
}

static ofono_bool_t android_wakelock_is_locked(struct wakelock *lock)
{
	if (!lock)
		return FALSE;

	return lock->acquisitions > 0;
}

struct wakelock_table driver = {
	.create = android_wakelock_create,
	.free = android_wakelock_free,
	.acquire = android_wakelock_acquire,
	.release = android_wakelock_release,
	.is_locked = android_wakelock_is_locked
};

static int android_wakelock_init(void)
{
	if (!file_exists(ANDROID_WAKELOCK_LOCK_PATH)) {
		ofono_warn("System does not support Android wakelocks.");
		return 0;
	}

	if (wakelock_plugin_register("android-wakelock", &driver) < 0) {
		ofono_error("Failed to register wakelock driver");
		return -EIO;
	}

	return 0;
}

static void android_wakelock_exit(void)
{
	wakelock_plugin_unregister();
}

OFONO_PLUGIN_DEFINE(android_wakelock, "Android Wakelock driver", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, android_wakelock_init, android_wakelock_exit)
