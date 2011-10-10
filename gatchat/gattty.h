/*
 *
 *  AT chat library with GLib integration
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

#ifndef __GATTTY_H
#define __GATTTY_H

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Opens a serial port given by tty.  If options is NULL, then the serial port
 * is opened in raw mode.  Otherwise the options are parsed and set accordingly
 *
 * The following keys / values are recognized (all strings)
 *
 * "Baud" - "300", "600", etc
 * "Stopbits" - "1", "2"
 * "Databits" - "7", "8"
 * "Parity" - "none", "odd", "even"
 * "XonXoff" - "on", "off"
 * "RtsCts" - "on", "off"
 * "Local" - "on", "off"
 * "Read" - "on, "off"
 */
GIOChannel *g_at_tty_open(const char *tty, GHashTable *options);

GIOChannel *g_at_tty_open_qcdm(const char *tty);

#ifdef __cplusplus
}
#endif

#endif /* __GATTTY_H */
