/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
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

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/audio-settings.h>

#include "gatchat.h"
#include "gatresult.h"

#include "ifxmodem.h"

static const char *none_prefix[] = { NULL };
static const char *xprogress_prefix[] = { "+XPROGRESS:", NULL };
static const char *xdrv_prefix[] = { "+XDRV:", NULL };

enum xdrv_destination {
	XDRV_DESTINATION_SPEECH_TX =	0,
	XDRV_DESTINATION_ANALOG_OUT =	1,
	XDRV_DESTINATION_I2SX_TX =	2,
	XDRV_DESTINATION_I2SY_TX =	3,
	XDRV_DESTINATION_PCM_GENERAL =	4,
};

enum xdrv_source {
	XDRV_SOURCE_SPEECH_RX =		0,
	XDRV_SOURCE_SPEECH_ANALOG_IN =	1,
	XDRV_SOURCE_DIGITAL_MIC_IN =	2,
	XDRV_SOURCE_I2SX_RX =		3,
	XDRV_SOURCE_I2SY_RX =		4,
	XDRV_SOURCE_SIMPLE_TONES =	5,
};

enum xdrv_sampling_rate {
	XDRV_SAMPLING_RATE_8KHZ =	0,
	XDRV_SAMPLING_RATE_11KHZ =	1,
	XDRV_SAMPLING_RATE_12KHZ =	2,
	XDRV_SAMPLING_RATE_16KHZ =	3,
	XDRV_SAMPLING_RATE_22KHZ =	4,
	XDRV_SAMPLING_RATE_24KHZ =	5,
	XDRV_SAMPLING_RATE_32KHZ =	6,
	XDRV_SAMPLING_RATE_44KHZ =	7,
	XDRV_SAMPLING_RATE_48KHZ =	8,
	XDRV_SAMPLING_RATE_96KHZ =	9,
	XDRV_SAMPLING_RATE_192KHZ =	10,
};

enum xdrv_sampling_width {
	XDRV_SAMPLING_WIDTH_16 =	0,
	XDRV_SAMPLING_WIDTH_18 =	1,
	XDRV_SAMPLING_WIDTH_20 =	2,
	XDRV_SAMPLING_WIDTH_24 =	3,
	XDRV_SAMPLING_WIDTH_32 =	4,
	XDRV_SAMPLING_WIDTH_48 =	5,
	XDRV_SAMPLING_WIDTH_64 =	6,
};

enum xdrv_i2s_mode {
	XDRV_I2S_MODE_MASTER =		0,
	XDRV_I2S_MODE_SLAVE =		1,
};

enum xdrv_i2s_clock {
	XDRV_I2S_CLOCK_0 =		0,
	XDRV_I2S_CLOCK_1 =		1,
};

enum xdrv_i2s_configuration_mode {
	XDRV_I2S_CONFIGURATION_MODE_UPDATE_ALL =	0,
	XDRV_I2S_CONFIGURATION_MODE_UPDATE_HW =		1,
	XDRV_I2S_CONFIGURATION_MODE_UPDATE_TRANSDUCER =	2,
};

enum xdrv_i2s_settings {
	XDRV_I2S_SETTINGS_NORMAL =	0,
	XDRV_I2S_SETTINGS_SPECIAL1 =	1,
	XDRV_I2S_SETTINGS_SPECIAL2 =	2,
};

enum xdrv_i2s_transmission_mode {
	XDRV_I2S_TRANSMISSION_MODE_PCM =	0,
	XDRV_I2S_TRANSMISSION_MODE_NORMAL =	1,
	XDRV_IS2_TRANSMISSION_MODE_PCM_BURST =	2,
};

enum xdrv_source_transducer {
	XDRV_SOURCE_TRANSDUCER_DEFAULT =		0,
	XDRV_SOURCE_TRANSDUCER_HANDSET =		1,
	XDRV_SOURCE_TRANSDUCER_HEADSET =		2,
	XDRV_SOURCE_TRANSDUCER_HF =			3,
	XDRV_SOURCE_TRANSDUCER_AUX =			4,
	XDRV_SOURCE_TRANSDUCER_TTY =			5,
	XDRV_SOURCE_TRANSDUCER_BLUETOOTH =		6,
	XDRV_SOURCE_TRANSDUCER_USER_DEFINED_15 =	21,
};

enum xdrv_dest_transducer {
	XDRV_DEST_TRANSDUCER_DEFAULT =		0,
	XDRV_DEST_TRANSDUCER_HANDSET =		1,
	XDRV_DEST_TRANSDUCER_HEADSET =		2,
	XDRV_DEST_TRANSDUCER_BACKSPEAKER =	3,
	XDRV_DEST_TRANSDUCER_TTY =		6,
	XDRV_DEST_TRANSDUCER_BLUETOOTH =	7,
	XDRV_DEST_TRANSDUCER_USER_DEFINED_15 =	22,
};

enum xdrv_audio_mode {
	XDRV_AUDIO_MODE_MONO =		0,
	XDRV_AUDIO_MODE_DUAL_MONO =	1,
	XDRV_AUDIO_MODE_STEREO =	2,
	XDRV_AUDIO_MODE_DUAL_MONO_R =	3,
	XDRV_AUDIO_MODE_DUAL_MONO_L =	4,
};

struct audio_settings_data {
	GAtChat *chat;
};

static inline void xdrv_enable_source(GAtChat *chat, enum xdrv_source src)
{
	char buf[256];

	sprintf(buf, "AT+XDRV=40,2,%i", src);
	g_at_chat_send(chat, buf, xdrv_prefix, NULL, NULL, NULL);
}

static inline void xdrv_disable_source(GAtChat *chat, enum xdrv_source src)
{
	char buf[256];

	sprintf(buf, "AT+XDRV=40,3,%i", src);
	g_at_chat_send(chat, buf, xdrv_prefix, NULL, NULL, NULL);
}

static inline void xdrv_configure_source(GAtChat *chat, enum xdrv_source src,
				enum xdrv_i2s_clock clock,
				enum xdrv_i2s_mode master_slave,
				enum xdrv_sampling_rate sample_rate,
				enum xdrv_sampling_width bits,
				enum xdrv_i2s_transmission_mode tx_mode,
				enum xdrv_i2s_settings settings,
				enum xdrv_audio_mode mode,
				enum xdrv_i2s_configuration_mode config_mode,
				enum xdrv_source_transducer transducer_mode)
{
	char buf[256];
	int ctx = 0; /* This is always 0 for now */

	sprintf(buf, "AT+XDRV=40,4,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i",
		src, ctx, clock, master_slave, sample_rate, bits,
		tx_mode, settings, mode, config_mode, transducer_mode);
	g_at_chat_send(chat, buf, xdrv_prefix, NULL, NULL, NULL);
}

static inline void xdrv_configure_destination(GAtChat *chat,
				enum xdrv_destination dest,
				enum xdrv_i2s_clock clock,
				enum xdrv_i2s_mode master_slave,
				enum xdrv_sampling_rate sample_rate,
				enum xdrv_sampling_width bits,
				enum xdrv_i2s_transmission_mode tx_mode,
				enum xdrv_i2s_settings settings,
				enum xdrv_audio_mode mode,
				enum xdrv_i2s_configuration_mode config_mode,
				enum xdrv_dest_transducer transducer_mode)
{
	char buf[256];
	int ctx = 0; /* This is always 0 for now */

	sprintf(buf, "AT+XDRV=40,5,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i,%i",
		dest, ctx, clock, master_slave, sample_rate, bits,
		tx_mode, settings, mode, config_mode, transducer_mode);
	g_at_chat_send(chat, buf, xdrv_prefix, NULL, NULL, NULL);
}

static inline void xdrv_set_destination_for_source(GAtChat *chat,
						enum xdrv_source src,
						enum xdrv_destination dest)
{
	char buf[256];

	sprintf(buf, "AT+XDRV=40,6,%i,%i", src, dest);
	g_at_chat_send(chat, buf, xdrv_prefix, NULL, NULL, NULL);
}

static inline void xdrv_set_destination_volume(GAtChat *chat,
						enum xdrv_destination dest,
						int volume)
{
	char buf[256];

	sprintf(buf, "AT+XDRV=40,8,%i,%i", dest, volume);
	g_at_chat_send(chat, buf, xdrv_prefix, NULL, NULL, NULL);
}

static void send_xdrv_setup_sequence(struct ofono_audio_settings *as)
{
	struct audio_settings_data *asd = ofono_audio_settings_get_data(as);

	/* Mute */
	xdrv_set_destination_volume(asd->chat, XDRV_DESTINATION_I2SX_TX, 0);
	xdrv_set_destination_volume(asd->chat, XDRV_DESTINATION_SPEECH_TX, 0);

	xdrv_set_destination_for_source(asd->chat, XDRV_SOURCE_SPEECH_RX,
					XDRV_DESTINATION_PCM_GENERAL);

	xdrv_disable_source(asd->chat, XDRV_SOURCE_I2SX_RX);
	xdrv_disable_source(asd->chat, XDRV_SOURCE_I2SY_RX);

	xdrv_configure_source(asd->chat, XDRV_SOURCE_I2SX_RX, XDRV_I2S_CLOCK_1,
				XDRV_I2S_MODE_MASTER, XDRV_SAMPLING_RATE_48KHZ,
				XDRV_SAMPLING_WIDTH_16,
				XDRV_I2S_TRANSMISSION_MODE_NORMAL,
				XDRV_I2S_SETTINGS_NORMAL,
				XDRV_AUDIO_MODE_STEREO,
				XDRV_I2S_CONFIGURATION_MODE_UPDATE_ALL,
				XDRV_SOURCE_TRANSDUCER_USER_DEFINED_15);
	xdrv_configure_destination(asd->chat, XDRV_DESTINATION_I2SX_TX,
					XDRV_I2S_CLOCK_1, XDRV_I2S_MODE_MASTER,
					XDRV_SAMPLING_RATE_48KHZ,
					XDRV_SAMPLING_WIDTH_16,
					XDRV_I2S_TRANSMISSION_MODE_NORMAL,
					XDRV_I2S_SETTINGS_NORMAL,
					XDRV_AUDIO_MODE_STEREO,
					XDRV_I2S_CONFIGURATION_MODE_UPDATE_ALL,
					XDRV_DEST_TRANSDUCER_USER_DEFINED_15);

	xdrv_configure_source(asd->chat, XDRV_SOURCE_I2SY_RX, XDRV_I2S_CLOCK_0,
				XDRV_I2S_MODE_MASTER, XDRV_SAMPLING_RATE_48KHZ,
				XDRV_SAMPLING_WIDTH_16,
				XDRV_I2S_TRANSMISSION_MODE_NORMAL,
				XDRV_I2S_SETTINGS_NORMAL,
				XDRV_AUDIO_MODE_STEREO,
				XDRV_I2S_CONFIGURATION_MODE_UPDATE_ALL,
				XDRV_SOURCE_TRANSDUCER_USER_DEFINED_15);
	xdrv_configure_destination(asd->chat, XDRV_DESTINATION_I2SY_TX,
					XDRV_I2S_CLOCK_0, XDRV_I2S_MODE_MASTER,
					XDRV_SAMPLING_RATE_48KHZ,
					XDRV_SAMPLING_WIDTH_16,
					XDRV_I2S_TRANSMISSION_MODE_NORMAL,
					XDRV_I2S_SETTINGS_NORMAL,
					XDRV_AUDIO_MODE_STEREO,
					XDRV_I2S_CONFIGURATION_MODE_UPDATE_ALL,
					XDRV_DEST_TRANSDUCER_USER_DEFINED_15);

	/* Seems unnecessary
	xdrv_set_destination_for_source(asd->chat, XDRV_SOURCE_SPEECH_RX,
					XDRV_DESTINATION_PCM_GENERAL);
	*/
	xdrv_set_destination_for_source(asd->chat, XDRV_SOURCE_I2SX_RX,
					XDRV_DESTINATION_SPEECH_TX);
	xdrv_set_destination_for_source(asd->chat, XDRV_SOURCE_I2SY_RX,
					XDRV_DESTINATION_I2SX_TX);
	xdrv_set_destination_for_source(asd->chat, XDRV_SOURCE_SIMPLE_TONES,
					XDRV_DESTINATION_I2SX_TX);

	xdrv_enable_source(asd->chat, XDRV_SOURCE_I2SX_RX);
	xdrv_enable_source(asd->chat, XDRV_SOURCE_I2SY_RX);

	xdrv_set_destination_for_source(asd->chat, XDRV_SOURCE_SPEECH_RX,
					XDRV_DESTINATION_I2SX_TX);

	/* Unmute */
	xdrv_set_destination_volume(asd->chat, XDRV_DESTINATION_I2SX_TX, 66);
	xdrv_set_destination_volume(asd->chat, XDRV_DESTINATION_SPEECH_TX, 100);
}

static void xprogress_notify(GAtResult *result, gpointer user_data)
{
	struct ofono_audio_settings *as = user_data;
	GAtResultIter iter;
	int id, status;

	g_at_result_iter_init(&iter, result);

	if (g_at_result_iter_next(&iter, "+XPROGRESS:") == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &id) == FALSE)
		return;

	if (g_at_result_iter_next_number(&iter, &status) == FALSE)
		return;

	switch (status) {
	case 0:
	case 1:
	case 4:
	case 9:
	case 10:
	case 11:
		ofono_audio_settings_active_notify(as, FALSE);
		break;
	case 2:
	case 3:
	case 5:
	case 6:
	case 7:
	case 8:
		ofono_audio_settings_active_notify(as, TRUE);
		break;
	}
}

static void xprogress_support_cb(gboolean ok, GAtResult *result,
						gpointer user_data)
{
	struct ofono_audio_settings *as = user_data;
	struct audio_settings_data *asd = ofono_audio_settings_get_data(as);

	if (!ok)
		return;

	g_at_chat_register(asd->chat, "+XPROGRESS:", xprogress_notify,
							FALSE, as, NULL);

	g_at_chat_send(asd->chat, "AT+XPROGRESS=1", none_prefix,
						NULL, NULL, NULL);

	ofono_audio_settings_register(as);

	send_xdrv_setup_sequence(as);
}

static int ifx_audio_settings_probe(struct ofono_audio_settings *as,
					unsigned int vendor, void *data)
{
	GAtChat *chat = data;
	struct audio_settings_data *asd;

	asd = g_try_new0(struct audio_settings_data, 1);
	if (asd == NULL)
		return -ENOMEM;

	asd->chat = g_at_chat_clone(chat);

	ofono_audio_settings_set_data(as, asd);

	g_at_chat_send(asd->chat, "AT+XPROGRESS=?", xprogress_prefix,
					xprogress_support_cb, as, NULL);

	return 0;
}

static void ifx_audio_settings_remove(struct ofono_audio_settings *as)
{
	struct audio_settings_data *asd = ofono_audio_settings_get_data(as);

	ofono_audio_settings_set_data(as, NULL);

	g_at_chat_unref(asd->chat);
	g_free(asd);
}

static struct ofono_audio_settings_driver driver = {
	.name		= "ifxmodem",
	.probe		= ifx_audio_settings_probe,
	.remove		= ifx_audio_settings_remove,
};

void ifx_audio_settings_init(void)
{
	ofono_audio_settings_driver_register(&driver);
}

void ifx_audio_settings_exit(void)
{
	ofono_audio_settings_driver_unregister(&driver);
}
