/*
 *
 *  AT chat library with GLib integration
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

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <termios.h>

#include <glib.h>

#include "gattty.h"

static gboolean set_baud(const char *baud, struct termios *ti)
{
	speed_t speed;

	if (g_str_equal(baud, "300"))
		speed = B300;
	else if (g_str_equal(baud, "1200"))
		speed = B1200;
	else if (g_str_equal(baud, "2400"))
		speed = B2400;
	else if (g_str_equal(baud, "4800"))
		speed = B4800;
	else if (g_str_equal(baud, "9600"))
		speed = B9600;
	else if (g_str_equal(baud, "19200"))
		speed = B19200;
	else if (g_str_equal(baud, "38400"))
		speed = B38400;
	else if (g_str_equal(baud, "57600"))
		speed = B57600;
	else if (g_str_equal(baud, "115200"))
		speed = B115200;
	else if (g_str_equal(baud, "230400"))
		speed = B230400;
	else if (g_str_equal(baud, "460800"))
		speed = B460800;
	else if (g_str_equal(baud, "500000"))
		speed = B500000;
	else if (g_str_equal(baud, "576000"))
		speed = B576000;
	else if (g_str_equal(baud, "921600"))
		speed = B921600;
	else if (g_str_equal(baud, "1000000"))
		speed = B1000000;
	else if (g_str_equal(baud, "1152000"))
		speed = B1152000;
	else if (g_str_equal(baud, "1500000"))
		speed = B1500000;
	else if (g_str_equal(baud, "2000000"))
		speed = B2000000;
#ifdef B2500000
	else if (g_str_equal(baud, "2500000"))
		speed = B2500000;
#endif
#ifdef B3000000
	else if (g_str_equal(baud, "3000000"))
		speed = B3000000;
#endif
#ifdef B3500000
	else if (g_str_equal(baud, "3500000"))
		speed = B3500000;
#endif
#ifdef B4000000
	else if (g_str_equal(baud, "4000000"))
		speed = B4000000;
#endif
	else
		return FALSE;

	cfsetospeed(ti, speed);
	cfsetispeed(ti, speed);

	return TRUE;
}

static gboolean set_read(const char *bits, struct termios *ti)
{
	if (g_str_equal(bits, "off"))
		ti->c_cflag &= ~(CREAD);
	else if (g_str_equal(bits, "on"))
		ti->c_cflag |= CREAD;
	else
		return FALSE;

	return TRUE;
}

static gboolean set_stop_bits(const char *bits, struct termios *ti)
{
	if (g_str_equal(bits, "1"))
		ti->c_cflag &= ~(CSTOPB);
	else if (g_str_equal(bits, "2"))
		ti->c_cflag |= CSTOPB;
	else
		return FALSE;

	return TRUE;
}

static gboolean set_data_bits(const char *bits, struct termios *ti)
{
	if (g_str_equal(bits, "7")) {
		ti->c_cflag &= ~(CSIZE);
		ti->c_cflag |= CS7;
	} else if (g_str_equal(bits, "8")) {
		ti->c_cflag &= ~(CSIZE);
		ti->c_cflag |= CS8;
	} else
		return FALSE;

	return TRUE;
}

static gboolean set_parity(const char *parity, struct termios *ti)
{
	if (g_str_equal(parity, "none"))
		ti->c_cflag &= ~(PARENB);
	else if (g_str_equal(parity, "even")) {
		ti->c_cflag |= PARENB;
		ti->c_cflag &= ~(PARODD);
	} else if (g_str_equal(parity, "odd")) {
		ti->c_cflag |= PARENB;
		ti->c_cflag |= PARODD;
	} else
		return FALSE;

	return TRUE;
}

static gboolean set_xonxoff(const char *xonxoff, struct termios *ti)
{
	if (g_str_equal(xonxoff, "on")) {
		ti->c_iflag |= (IXON | IXOFF | IXANY);
		ti->c_cc[VSTART] = 17;
		ti->c_cc[VSTOP] = 19;
	} else if (g_str_equal(xonxoff, "off"))
		ti->c_iflag &= ~(IXON | IXOFF | IXANY);
	else
		return FALSE;

	return TRUE;
}

static gboolean set_rtscts(const char *rtscts, struct termios *ti)
{
	if (g_str_equal(rtscts, "on"))
		ti->c_cflag |= CRTSCTS;
	else if (g_str_equal(rtscts, "off"))
		ti->c_cflag &= ~(CRTSCTS);
	else
		return FALSE;

	return TRUE;
}

static gboolean set_local(const char *local, struct termios *ti)
{
	if (g_str_equal(local, "on"))
		ti->c_cflag |= CLOCAL;
	else if (g_str_equal(local, "off"))
		ti->c_cflag &= ~(CLOCAL);
	else
		return FALSE;

	return TRUE;
}

static int open_device(const char *tty, GHashTable *options)
{
	struct termios ti;
	int fd;

	/* Switch TTY to raw mode */
	memset(&ti, 0, sizeof(ti));
	cfmakeraw(&ti);

	if (options) {
		GHashTableIter iter;
		const char *key;
		const char *value;

		g_hash_table_iter_init (&iter, options);
		while (g_hash_table_iter_next(&iter, (void *) &key,
							(void *) &value)) {
			gboolean ok = FALSE;

			if (g_str_equal(key, "Baud"))
				ok = set_baud(value, &ti);
			else if (g_str_equal(key, "StopBits"))
				ok = set_stop_bits(value, &ti);
			else if (g_str_equal(key, "DataBits"))
				ok = set_data_bits(value, &ti);
			else if (g_str_equal(key, "Parity"))
				ok = set_parity(value, &ti);
			else if (g_str_equal(key, "XonXoff"))
				ok = set_xonxoff(value, &ti);
			else if (g_str_equal(key, "RtsCts"))
				ok = set_rtscts(value, &ti);
			else if (g_str_equal(key, "Local"))
				ok = set_local(value, &ti);
			else if (g_str_equal(key, "Read"))
				ok = set_read(value, &ti);

			if (ok == FALSE)
				return -1;
		}
	}

	fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0)
		return -1;

	tcflush(fd, TCIOFLUSH);
	tcsetattr(fd, TCSANOW, &ti);

	return fd;
}

GIOChannel *g_at_tty_open(const char *tty, GHashTable *options)
{
	GIOChannel *channel;
	int fd;

	fd = open_device(tty, options);
	if (fd < 0)
		return NULL;

	channel = g_io_channel_unix_new(fd);
	if (channel == NULL) {
		close(fd);
		return NULL;
	}

	g_io_channel_set_close_on_unref(channel, TRUE);

	return channel;
}

GIOChannel *g_at_tty_open_qcdm(const char *tty)
{
	GIOChannel *channel;
	struct termios ti;
	int fd;

        fd = open(tty, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0)
                return NULL;

	/* Switch TTY to raw mode */
	memset(&ti, 0, sizeof(ti));
	cfmakeraw(&ti);

	/* No parity, 1 stop bit */
	ti.c_cflag &= ~(CSIZE | CSTOPB | PARENB);
	ti.c_cflag |= (B115200 | CS8);

	if (tcsetattr (fd, TCSANOW, &ti) < 0) {
		close(fd);
		return NULL;
	}

	channel = g_io_channel_unix_new(fd);
	if (channel == NULL) {
		close(fd);
		return NULL;
	}

	g_io_channel_set_close_on_unref(channel, TRUE);

	return channel;
}
