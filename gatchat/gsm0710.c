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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <alloca.h>
#include <string.h>
#include <stdio.h>

#include "gsm0710.h"

/* Frame types and subtypes */
#define GSM0710_OPEN_CHANNEL		0x3F
#define GSM0710_CLOSE_CHANNEL		0x53
#define GSM0710_DATA			0xEF
#define GSM0710_DATA_ALT		0x03
#define GSM0710_STATUS_SET		0xE3
#define GSM0710_STATUS_ACK		0xE1
#define GSM0710_TERMINATE_BYTE1		0xC3
#define GSM0710_TERMINATE_BYTE2		0x01

/* Initialize a GSM 07.10 context, in preparation for startup */
void gsm0710_initialize(struct gsm0710_context *ctx)
{
	ctx->mode = GSM0710_MODE_BASIC;
	ctx->frame_size = GSM0710_DEFAULT_FRAME_SIZE;
	ctx->port_speed = 115200;
	ctx->server = 0;
	ctx->buffer_used = 0;
	memset(ctx->used_channels, 0, sizeof(ctx->used_channels));
	ctx->user_data = NULL;
	ctx->read = NULL;
	ctx->write = NULL;
	ctx->deliver_data = NULL;
	ctx->deliver_status = NULL;
	ctx->debug_message = NULL;
	ctx->open_channel = NULL;
	ctx->close_channel = NULL;
	ctx->terminate = NULL;
	ctx->packet_filter = NULL;
}

/* Determine if a channel is in use */
static int is_channel_used(struct gsm0710_context *ctx, int channel)
{
	int index = channel / 32;
	return ((ctx->used_channels[index] & (1L << (channel % 32))) != 0);
}

/* Mark a channel as used */
static void mark_channel_used(struct gsm0710_context *ctx, int channel)
{
	int index = channel / 32;
	ctx->used_channels[index] |= (1L << (channel % 32));
}

/* Mark a channel as unused */
static void mark_channel_unused(struct gsm0710_context *ctx, int channel)
{
	int index = channel / 32;
	ctx->used_channels[index] &= ~(1L << (channel % 32));
}

/* Write a debug message */
static void gsm0710_debug(struct gsm0710_context *ctx, const char *msg)
{
	if (ctx->debug_message)
		ctx->debug_message(ctx, msg);
}

static const unsigned char crc_table[256] = {
	0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75,
	0x0E, 0x9F, 0xED, 0x7C, 0x09, 0x98, 0xEA, 0x7B,
	0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A, 0xF8, 0x69,
	0x12, 0x83, 0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67,
	0x38, 0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC, 0x4D,
	0x36, 0xA7, 0xD5, 0x44, 0x31, 0xA0, 0xD2, 0x43,
	0x24, 0xB5, 0xC7, 0x56, 0x23, 0xB2, 0xC0, 0x51,
	0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F,
	0x70, 0xE1, 0x93, 0x02, 0x77, 0xE6, 0x94, 0x05,
	0x7E, 0xEF, 0x9D, 0x0C, 0x79, 0xE8, 0x9A, 0x0B,
	0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19,
	0x62, 0xF3, 0x81, 0x10, 0x65, 0xF4, 0x86, 0x17,
	0x48, 0xD9, 0xAB, 0x3A, 0x4F, 0xDE, 0xAC, 0x3D,
	0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0, 0xA2, 0x33,
	0x54, 0xC5, 0xB7, 0x26, 0x53, 0xC2, 0xB0, 0x21,
	0x5A, 0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F,
	0xE0, 0x71, 0x03, 0x92, 0xE7, 0x76, 0x04, 0x95,
	0xEE, 0x7F, 0x0D, 0x9C, 0xE9, 0x78, 0x0A, 0x9B,
	0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89,
	0xF2, 0x63, 0x11, 0x80, 0xF5, 0x64, 0x16, 0x87,
	0xD8, 0x49, 0x3B, 0xAA, 0xDF, 0x4E, 0x3C, 0xAD,
	0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3,
	0xC4, 0x55, 0x27, 0xB6, 0xC3, 0x52, 0x20, 0xB1,
	0xCA, 0x5B, 0x29, 0xB8, 0xCD, 0x5C, 0x2E, 0xBF,
	0x90, 0x01, 0x73, 0xE2, 0x97, 0x06, 0x74, 0xE5,
	0x9E, 0x0F, 0x7D, 0xEC, 0x99, 0x08, 0x7A, 0xEB,
	0x8C, 0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9,
	0x82, 0x13, 0x61, 0xF0, 0x85, 0x14, 0x66, 0xF7,
	0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C, 0xDD,
	0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3,
	0xB4, 0x25, 0x57, 0xC6, 0xB3, 0x22, 0x50, 0xC1,
	0xBA, 0x2B, 0x59, 0xC8, 0xBD, 0x2C, 0x5E, 0xCF
};

static unsigned char gsm0710_compute_crc(const unsigned char *data, int len)
{
	int sum = 0xFF;
	while (len > 0) {
		sum = crc_table[(sum ^ *data++) & 0xFF];
		--len;
	}
	return (~sum & 0xFF);
}

/* Write a raw GSM 07.10 frame to the underlying device */
static void gsm0710_write_frame(struct gsm0710_context *ctx, int channel,
				int type, const unsigned char *data, int len)
{
	unsigned char *frame = alloca(ctx->frame_size * 2 + 8);
	int size;
	if (len > ctx->frame_size)
		len = ctx->frame_size;
	if (ctx->mode) {
		int temp, crc;
		frame[0] = 0x7E;
		frame[1] = ((channel << 2) | 0x03);
		frame[2] = type;
		crc = gsm0710_compute_crc(frame + 1, 2);
		if (type == 0x7E || type == 0x7D) {
			/* Need to quote the type field now that crc has been computed */
			frame[2] = 0x7D;
			frame[3] = (type ^ 0x20);
			size = 4;
		} else {
			size = 3;
		}
		while (len > 0) {
			temp = *data++ & 0xFF;
			--len;
			if (temp != 0x7E && temp != 0x7D) {
				frame[size++] = temp;
			} else {
				frame[size++] = 0x7D;
				frame[size++] = (temp ^ 0x20);
			}
		}
		if (crc != 0x7E && crc != 0x7D) {
			frame[size++] = crc;
		} else {
			frame[size++] = 0x7D;
			frame[size++] = (crc ^ 0x20);
		}
		frame[size++] = 0x7E;
	} else {
		int header_size;
		frame[0] = 0xF9;
		frame[1] = ((channel << 2) | 0x03);
		frame[2] = type;
		if (len <= 127) {
			frame[3] = ((len << 1) | 0x01);
			header_size = size = 4;
		} else {
			frame[3] = (len << 1);
			frame[4] = (len >> 7);
			header_size = size = 5;
		}
		if (len > 0) {
			memcpy(frame + size, data, len);
			size += len;
		}
		/* Note: GSM 07.10 says that the CRC is only computed over the header */
		frame[size++] = gsm0710_compute_crc(frame + 1, header_size - 1);
		frame[size++] = 0xF9;
	}
	if (ctx->write)
		ctx->write(ctx, frame, size);
}

/* Start up the GSM 07.10 session on the underlying device.
   The underlying device is assumed to already be in
   multiplexing mode.  Returns zero on failure */
int gsm0710_startup(struct gsm0710_context *ctx)
{
	/* Discard any data in the buffer, in case of restart */
	ctx->buffer_used = 0;

	/* Open the control channel */
	gsm0710_write_frame(ctx, 0, GSM0710_OPEN_CHANNEL, NULL, 0);

	return 1;
}

/* Shut down the GSM 07.10 session, closing all channels */
void gsm0710_shutdown(struct gsm0710_context *ctx)
{
	static const unsigned char terminate[2] = { GSM0710_TERMINATE_BYTE1,
						GSM0710_TERMINATE_BYTE2 };
	if (!ctx->server) {
		int channel;
		for (channel = 1; channel <= GSM0710_MAX_CHANNELS; ++channel) {
			if (is_channel_used(ctx, channel)) {
				gsm0710_write_frame(ctx, channel,
						GSM0710_CLOSE_CHANNEL, NULL, 0);
			}
		}
		gsm0710_write_frame(ctx, 0, GSM0710_DATA, terminate, 2);
	}
	memset(ctx->used_channels, 0, sizeof(ctx->used_channels));
}

/* Open a specific channel.  Returns non-zero if successful */
int gsm0710_open_channel(struct gsm0710_context *ctx, int channel)
{
	if (channel <= 0 || channel > GSM0710_MAX_CHANNELS)
		return 0;	/* Invalid channel number */
	if (is_channel_used(ctx, channel))
		return 1;	/* Channel is already open */
	mark_channel_used(ctx, channel);
	if (!ctx->server)
		gsm0710_write_frame(ctx, channel,
					GSM0710_OPEN_CHANNEL, NULL, 0);
	return 1;
}

/* Close a specific channel */
void gsm0710_close_channel(struct gsm0710_context *ctx, int channel)
{
	if (channel <= 0 || channel > GSM0710_MAX_CHANNELS)
		return;		/* Invalid channel number */
	if (!is_channel_used(ctx, channel))
		return;		/* Channel is already closed */
	mark_channel_unused(ctx, channel);
	if (!ctx->server)
		gsm0710_write_frame(ctx, channel,
					GSM0710_CLOSE_CHANNEL, NULL, 0);
}

/* Determine if a specific channel is open */
int gsm0710_is_channel_open(struct gsm0710_context *ctx, int channel)
{
	if (channel <= 0 || channel > GSM0710_MAX_CHANNELS)
		return 0;	/* Invalid channel number */
	return is_channel_used(ctx, channel);
}

/* Process an incoming GSM 07.10 packet */
static int gsm0710_packet(struct gsm0710_context *ctx, int channel, int type,
					const unsigned char *data, int len)
{
	if (ctx->packet_filter &&
			ctx->packet_filter(ctx, channel, type, data, len)) {
		/* The filter has extracted and processed the packet */
		return 1;
	}
	if (type == 0xEF || type == 0x03) {

		if (channel >= 1 && channel <= GSM0710_MAX_CHANNELS &&
					is_channel_used(ctx, channel)) {
			/* Ordinary data packet */
			if (ctx->deliver_data)
				ctx->deliver_data(ctx, channel, data, len);
		} else if (channel == 0) {
			/* An embedded command or response on channel 0 */
			if (len >= 2 && data[0] == GSM0710_STATUS_SET) {
				return gsm0710_packet(ctx, channel,
							GSM0710_STATUS_ACK,
							data + 2, len - 2);
			} else if (len >= 2 && data[0] == 0xC3 && ctx->server) {
				/* Incoming terminate request on server side */
				for (channel = 1; channel <= GSM0710_MAX_CHANNELS; ++channel) {
					if (is_channel_used(ctx, channel)) {
						if (ctx->close_channel)
							ctx->close_channel(ctx, channel);
					}
				}
				memset(ctx->used_channels, 0,
						sizeof(ctx->used_channels));
				if (ctx->terminate)
					ctx->terminate(ctx);
				return 0;
			} else if (len >= 2 && data[0] == 0x43) {
				/* Test command from other side - send the same bytes back */
				unsigned char *resp = alloca(len);
				memcpy(resp, data, len);
				resp[0] = 0x41;	/* Clear the C/R bit in the response */
				gsm0710_write_frame(ctx, 0, GSM0710_DATA,
								resp, len);
			}
		}

	} else if (type == GSM0710_STATUS_ACK && channel == 0) {

		unsigned char resp[33];

		/* Status change message */
		if (len >= 2) {
			/* Handle status changes on other channels */
			channel = ((data[0] & 0xFC) >> 2);
			if (channel >= 1 && channel <= GSM0710_MAX_CHANNELS &&
						is_channel_used(ctx, channel)) {
				if (ctx->deliver_status)
					ctx->deliver_status(ctx, channel,
							data[1] & 0xFF);
			}
		}

		/* Send the response to the status change request to ACK it */
		gsm0710_debug(ctx,
				"received status line signal, sending response");
		if (len > 31)
			len = 31;
		resp[0] = GSM0710_STATUS_ACK;
		resp[1] = ((len << 1) | 0x01);
		memcpy(resp + 2, data, len);
		gsm0710_write_frame(ctx, 0, GSM0710_DATA, resp, len + 2);

	} else if (type == (0x3F & 0xEF) && ctx->server) {

		/* Incoming channel open request on server side */
		if (channel >= 1 && channel <= GSM0710_MAX_CHANNELS) {
			if (!is_channel_used(ctx, channel)) {
				mark_channel_used(ctx, channel);
				if (ctx->open_channel)
					ctx->open_channel(ctx, channel);
			}
		}

	} else if (type == (0x53 & 0xEF) && ctx->server) {

		/* Incoming channel close request on server side */
		if (channel >= 1 && channel <= GSM0710_MAX_CHANNELS) {
			if (is_channel_used(ctx, channel)) {
				mark_channel_unused(ctx, channel);
				if (ctx->close_channel)
					ctx->close_channel(ctx, channel);
			}
		}

	}
	return 1;
}

/* Function that is called when the underlying device is ready to be read.
   A callback will be made to ctx->read to get the data for processing */
void gsm0710_ready_read(struct gsm0710_context *ctx)
{
	int len, posn, posn2, header_size, channel, type;

	/* Read more data from the underlying serial device */
	if (!ctx->read)
		return;
	len = ctx->read(ctx, ctx->buffer + ctx->buffer_used,
			sizeof(ctx->buffer) - ctx->buffer_used);
	if (len <= 0)
		return;

	/* Update the buffer size */
	ctx->buffer_used += len;

	/* Break the incoming data up into packets */
	posn = 0;
	while (posn < ctx->buffer_used) {
		if (ctx->buffer[posn] == 0xF9) {

			/* Basic format: skip additional 0xF9 bytes between frames */
			while ((posn + 1) < ctx->buffer_used &&
						ctx->buffer[posn + 1] == 0xF9) {
				++posn;
			}

			/* We need at least 4 bytes for the header */
			if ((posn + 4) > ctx->buffer_used)
				break;

			/* The low bit of the second byte should be 1,
			   which indicates a short channel number */
			if ((ctx->buffer[posn + 1] & 0x01) == 0) {
				++posn;
				continue;
			}

			/* Get the packet length and validate it */
			len = (ctx->buffer[posn + 3] >> 1) & 0x7F;
			if ((ctx->buffer[posn + 3] & 0x01) != 0) {
				/* Single-byte length indication */
				header_size = 3;
			} else {
				/* Double-byte length indication */
				if ((posn + 5) > ctx->buffer_used)
					break;
				len |= ((int)(ctx->buffer[posn + 4])) << 7;
				header_size = 4;
			}
			if ((posn + header_size + 2 + len) > ctx->buffer_used)
				break;

			/* Verify the packet header checksum */
			if (((gsm0710_compute_crc(ctx->buffer + posn + 1,
					header_size) ^ ctx->buffer[posn + len + header_size + 1]) & 0xFF) != 0) {
				gsm0710_debug(ctx,
					"*** GSM 07.10 checksum check failed ***");
				posn += len + header_size + 2;
				continue;
			}

			/* Get the channel number and packet type from the header */
			channel = (ctx->buffer[posn + 1] >> 2) & 0x3F;
			type = ctx->buffer[posn + 2] & 0xEF;	/* Strip "PF" bit */

			/* Dispatch data packets to the appropriate channel */
			if (!gsm0710_packet(ctx, channel, type,
					ctx->buffer + posn + header_size + 1, len)) {
				/* Session has been terminated */
				ctx->buffer_used = 0;
				return;
			}
			posn += len + header_size + 2;

		} else if (ctx->buffer[posn] == 0x7E) {

			/* Advanced format: skip additional 0x7E bytes between frames */
			while ((posn + 1) < ctx->buffer_used &&
					ctx->buffer[posn + 1] == 0x7E) {
				++posn;
			}

			/* Search for the end of the packet (the next 0x7E byte) */
			len = posn + 1;
			while (len < ctx->buffer_used &&
						ctx->buffer[len] != 0x7E) {
				++len;
			}
			if (len >= ctx->buffer_used) {
				/* There are insufficient bytes for a packet at present */
				if (posn == 0 && len >= (int)sizeof(ctx->buffer)) {
					/* The buffer is full and we were unable to find a
					   legitimate packet.  Discard the buffer and restart */
					posn = len;
				}
				break;
			}

			/* Undo control byte quoting in the packet */
			posn2 = 0;
			++posn;
			while (posn < len) {
				if (ctx->buffer[posn] == 0x7D) {
					++posn;
					if (posn >= len)
						break;
					ctx->buffer[posn2++] = (ctx->buffer[posn++] ^ 0x20);
				} else {
					ctx->buffer[posn2++] = ctx->buffer[posn++];
				}
			}

			/* Validate the checksum on the packet header */
			if (posn2 >= 3) {
				if (((gsm0710_compute_crc(ctx->buffer, 2) ^
						ctx->buffer[posn2 - 1]) & 0xFF) != 0) {
					gsm0710_debug(ctx,
							"*** GSM 07.10 advanced checksum "
							"check failed ***");
					continue;
				}
			} else {
				gsm0710_debug(ctx,
						"*** GSM 07.10 advanced packet "
						"is too small ***");
				continue;
			}

			/* Decode and dispatch the packet */
			channel = (ctx->buffer[0] >> 2) & 0x3F;
			type = ctx->buffer[1] & 0xEF;	/* Strip "PF" bit */
			if (!gsm0710_packet(ctx, channel, type,
						ctx->buffer + 2, posn2 - 3)) {
				/* Session has been terminated */
				ctx->buffer_used = 0;
				return;
			}

		} else {
			++posn;
		}
	}
	if (posn < ctx->buffer_used) {
		memmove(ctx->buffer, ctx->buffer + posn,
			ctx->buffer_used - posn);
		ctx->buffer_used -= posn;
	} else {
		ctx->buffer_used = 0;
	}
}

/* Write a block of data to the the underlying device.  It will be split
   into several frames according to the frame size, if necessary */
void gsm0710_write_data(struct gsm0710_context *ctx, int channel,
						const void *data, int len)
{
	int temp;
	while (len > 0) {
		temp = len;
		if (temp > ctx->frame_size)
			temp = ctx->frame_size;
		gsm0710_write_frame(ctx, channel, GSM0710_DATA, data, temp);
		data = ((const unsigned char *) data) + temp;
		len -= temp;
	}
}

/* Set the modem status lines on a channel */
void gsm0710_set_status(struct gsm0710_context *ctx, int channel, int status)
{
	unsigned char data[4];
	data[0] = GSM0710_STATUS_SET;
	data[1] = 0x03;
	data[2] = ((channel << 2) | 0x03);
	data[3] = status;
	gsm0710_write_frame(ctx, 0, GSM0710_DATA, data, 4);
}
