/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2014 Canonical Ltd.
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
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/inotify.h>

#include <glib.h>
#include <string.h>
#include <stdio.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/modem.h>
#include <ofono/log.h>

#define RIL_SOCKET_FOLDER "/dev/socket/"

struct rilmodem_info {
	const char *ril_type;
	int slot;
	char *socket;
};

static GSList *g_modem_list;
static GIOChannel *g_notif_chann;
static guint g_gio_watch;
static int g_dev_watch;
static GSList *g_socket_list;

static int create_rilmodem(const char *ril_type, int slot, const char *socket)
{
	struct ofono_modem *modem;
	char dev_name[64];
	int retval;

	snprintf(dev_name, sizeof(dev_name), "ril_%d", slot);

	/* Currently there is only one ril implementation, create always */
	modem = ofono_modem_create(dev_name, ril_type);
	if (modem == NULL) {
		DBG("ofono_modem_create failed for type %s", ril_type);
		return -ENODEV;
	}

	ofono_modem_set_integer(modem, "Slot", slot);

	/*
	 * We assume all slots of this type belong to a single physical modem
	 * that works in standby mode.
	 */
	ofono_modem_set_string(modem, "StandbyGroup", ril_type);

	ofono_modem_set_string(modem, "Socket", socket);

	/* This causes driver->probe() to be called... */
	retval = ofono_modem_register(modem);
	if (retval != 0) {
		ofono_error("%s: ofono_modem_register returned: %d",
				__func__, retval);
		ofono_modem_remove(modem);
		return retval;
	}

	g_modem_list = g_slist_prepend(g_modem_list, modem);

	/*
	 * kickstart the modem:
	 * causes core modem code to call
	 * - set_powered(TRUE) - which in turn
	 *   calls driver->enable()
	 *
	 * - driver->pre_sim()
	 *
	 * Could also be done via:
	 *
	 * - a DBus call to SetProperties w/"Powered=TRUE" *1
	 * - sim_state_watch ( handles SIM removal? LOCKED states? **2
	 * - ofono_modem_set_powered()
	 */
	ofono_modem_reset(modem);

	return 0;
}

static int probe_rilmodem(const char *ril_type, int slot)
{
	char *socket;
	int retval;

	/* AOSP has socket path "rild", "rild2"..., while others may differ */
	if (slot != 0)
		if (strcmp("qcom_msim", ril_type) == 0)
			socket = g_strdup_printf(RIL_SOCKET_FOLDER "rild%d",
								slot);
		else
			socket = g_strdup_printf(RIL_SOCKET_FOLDER "rild%d",
								slot + 1);
	else
		socket = g_strdup(RIL_SOCKET_FOLDER "rild");

	retval = access(socket, R_OK | W_OK);
	if (retval < 0) {
		if (errno == ENOENT) {
			struct rilmodem_info *info = g_malloc0(sizeof(*info));

			info->ril_type = ril_type;
			info->slot = slot;
			info->socket = socket;
			g_socket_list = g_slist_prepend(g_socket_list, info);

			goto exit;
		} else {
			ofono_info("Error probing %s: %s",
						socket, strerror(errno));
			g_free(socket);
			goto exit;
		}
	}

	ofono_info("Creating rilmodem with socket %s", socket);
	retval = create_rilmodem(ril_type, slot, socket);
	g_free(socket);

exit:
	return retval;
}

static void destroy_rilinfo(gpointer data)
{
	struct rilmodem_info *info = data;

	g_free(info->socket);
	g_free(info);
}

static gboolean inotify_event(GIOChannel *channel, GIOCondition cond,
								gpointer data)
{
	char buf[4096]
		__attribute__((aligned(__alignof__(struct inotify_event))));
	gsize len = 0, i = 0;
	GIOStatus status;

	if (G_IO_IN != cond)
		return FALSE;

	status = g_io_channel_read_chars(channel, buf, sizeof(buf), &len, NULL);
	if (status != G_IO_STATUS_NORMAL) {
		ofono_error("error reading from inotify");
		return FALSE;
	}

	while (i < len) {
		struct inotify_event *event = (void *) &buf[i];
		GSList *list;

		if (!event) {
			ofono_error("error, corrupt inotify_event data");
			return FALSE;
		}

		/*
		 * Note that event->name contains extra NULL bytes to force
		 * alignment of events, so event->len is aligned here
		 */
		i += sizeof(struct inotify_event) + event->len;

		if (event->len == 0)
			continue;

		/* Search name in list */
		for (list = g_socket_list; list; list = list->next) {
			struct rilmodem_info *info = list->data;

			/* event->name does not include the folder name */
			if (strcmp(info->socket + sizeof(RIL_SOCKET_FOLDER) - 1,
							event->name) != 0)
				continue;

			ofono_info("Creating rilmodem with socket %s (inotify)",
								info->socket);
			create_rilmodem(info->ril_type,
						info->slot, info->socket);
			g_socket_list =
				g_slist_delete_link(g_socket_list, list);
			destroy_rilinfo(info);
			break;
		}
	}

	return TRUE;
}

static void free_watches(void)
{
	if (g_dev_watch > 0)
		inotify_rm_watch(g_io_channel_unix_get_fd(g_notif_chann),
								g_dev_watch);

	if (g_notif_chann != NULL) {
		if (g_gio_watch > 0)
			g_source_remove(g_gio_watch);
		g_io_channel_unref(g_notif_chann);
	}

	g_dev_watch = 0;
	g_notif_chann = NULL;
	g_gio_watch = 0;
}

static void start_inotify(void)
{
	int fd;

	fd = inotify_init();
	if (fd < 0) {
		ofono_error("inotify_init failed: %s", strerror(errno));
		goto error;
	}

	g_notif_chann = g_io_channel_unix_new(fd);
	if (g_notif_chann == NULL) {
		ofono_error("error creating gio channel for inotify");
		goto error;
	}

	g_io_channel_set_close_on_unref(g_notif_chann, TRUE);

	if (g_io_channel_set_encoding(g_notif_chann, NULL, NULL)
							!= G_IO_STATUS_NORMAL) {
		ofono_error("error setting encoding of gio for inotify");
		goto error;
	}

	g_io_channel_set_buffered(g_notif_chann, FALSE);
	g_gio_watch =
		g_io_add_watch(g_notif_chann, G_IO_IN, inotify_event, NULL);

	g_dev_watch = inotify_add_watch(fd, RIL_SOCKET_FOLDER, IN_CREATE);
	if (g_dev_watch < 0) {
		ofono_error("error adding watch for ril sockets");
		goto error;
	}

	return;

error:
	free_watches();
	return;
}

static int detect_init(void)
{
	const char *ril_type;
	const char *multi_sim;
	int num_slots = 1;
	int i;

	ril_type = getenv("OFONO_RIL_DEVICE");
	if (ril_type == NULL)
		return 0;

	/* Check for multi-SIM support */
	multi_sim = getenv("OFONO_RIL_NUM_SIM_SLOTS");
	if (multi_sim != NULL && *multi_sim != '\0') {
		int env_slots;
		char *endp;

		env_slots = (int) strtoul(multi_sim, &endp, 10);
		if (*endp == '\0')
			num_slots = env_slots;
	}

	ofono_info("RILDEV searching modem type %s, %d SIM slot(s)",
							ril_type, num_slots);

	/* Add watch as sockets might take a while to create */
	start_inotify();

	for (i = 0; i < num_slots; ++i)
		probe_rilmodem(ril_type, i);

	return 0;
}

static void detect_exit(void)
{
	GSList *list;

	for (list = g_modem_list; list; list = list->next) {
		struct ofono_modem *modem = list->data;

		ofono_modem_remove(modem);
	}

	g_slist_free(g_modem_list);
	g_modem_list = NULL;

	free_watches();

	g_slist_free_full(g_socket_list, destroy_rilinfo);
	g_socket_list = NULL;
}

OFONO_PLUGIN_DEFINE(rildev, "ril type detection", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT, detect_init, detect_exit)
