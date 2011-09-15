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

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <glib.h>
#include <gatchat.h>
#include <gatresult.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/handsfree.h>

#include "hfpmodem.h"
#include "slc.h"

struct hf_data {
	GAtChat *chat;
};

static gboolean hfp_handsfree_register(gpointer user_data)
{
	struct ofono_handsfree *hf = user_data;

	ofono_handsfree_register(hf);

	return FALSE;
}

static int hfp_handsfree_probe(struct ofono_handsfree *hf,
				unsigned int vendor, void *data)
{
	struct hfp_slc_info *info = data;
	struct hf_data *hd;

	DBG("");
	hd = g_new0(struct hf_data, 1);
	hd->chat = g_at_chat_clone(info->chat);

	ofono_handsfree_set_data(hf, hd);

	g_idle_add(hfp_handsfree_register, hf);

	return 0;
}

static void hfp_handsfree_remove(struct ofono_handsfree *hf)
{
	struct hf_data *hd = ofono_handsfree_get_data(hf);

	ofono_handsfree_set_data(hf, NULL);

	g_at_chat_unref(hd->chat);
	g_free(hd);
}

static struct ofono_handsfree_driver driver = {
	.name			= "hfpmodem",
	.probe			= hfp_handsfree_probe,
	.remove			= hfp_handsfree_remove,
};

void hfp_handsfree_init(void)
{
	ofono_handsfree_driver_register(&driver);
}

void hfp_handsfree_exit(void)
{
	ofono_handsfree_driver_unregister(&driver);
}
