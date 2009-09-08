/*
 *
 *  AT chat library with GLib integration
 *
 *  Copyright (C) 2009  Intel Corporation. All rights reserved.
 *  Copyright (C) 2009  Trolltech ASA.
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

#ifndef GSM0710_P_H
#define GSM0710_P_H

#ifdef __cplusplus
extern "C" {
#endif

#define GSM0710_BUFFER_SIZE		4096
#define GSM0710_DEFAULT_FRAME_SIZE	31
#define GSM0710_MAX_CHANNELS		63

/* Multiplexer modes */
#define GSM0710_MODE_BASIC		0
#define GSM0710_MODE_ADVANCED		1

/* Status flags */
#define GSM0710_FC			0x02
#define GSM0710_DTR			0x04
#define GSM0710_DSR			0x04
#define GSM0710_RTS			0x08
#define GSM0710_CTS			0x08
#define GSM0710_DCD			0x80

struct gsm0710_context
{
	/* GSM 07.10 implementation details */
	int mode;
	int frame_size;
	int port_speed;
	int server;
	unsigned char buffer[GSM0710_BUFFER_SIZE];
	int buffer_used;
	unsigned long used_channels[(GSM0710_MAX_CHANNELS + 31) / 32];

	/* Hooks to other levels */
	void *user_data;
	int (*read)(struct gsm0710_context *ctx, void *data, int len);
	int (*write)(struct gsm0710_context *ctx, const void *data, int len);
	void (*deliver_data)(struct gsm0710_context *ctx, int channel,
						const void *data, int len);
	void (*deliver_status)(struct gsm0710_context *ctx,
						int channel, int status);
	void (*debug_message)(struct gsm0710_context *ctx, const char *msg);
	void (*open_channel)(struct gsm0710_context *ctx, int channel);
	void (*close_channel)(struct gsm0710_context *ctx, int channel);
	void (*terminate)(struct gsm0710_context *ctx);
	int (*packet_filter)(struct gsm0710_context *ctx, int channel,
				int type, const unsigned char *data, int len);
};

void gsm0710_initialize(struct gsm0710_context *ctx);
int gsm0710_startup(struct gsm0710_context *ctx);
void gsm0710_shutdown(struct gsm0710_context *ctx);
int gsm0710_open_channel(struct gsm0710_context *ctx, int channel);
void gsm0710_close_channel(struct gsm0710_context *ctx, int channel);
int gsm0710_is_channel_open(struct gsm0710_context *ctx, int channel);
void gsm0710_ready_read(struct gsm0710_context *ctx);
void gsm0710_write_data(struct gsm0710_context *ctx, int channel,
						const void *data, int len);
void gsm0710_set_status(struct gsm0710_context *ctx, int channel, int status);

#ifdef __cplusplus
};
#endif

#endif
