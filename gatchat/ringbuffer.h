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

struct ring_buffer;

/*!
 * Creates a new ring buffer with capacity size
 */
struct ring_buffer *ring_buffer_new(unsigned int size);

/*!
 * Frees the resources allocated for the ring buffer
 */
void ring_buffer_free(struct ring_buffer *buf);

/*!
 * Returns the capacity of the ring buffer
 */
int ring_buffer_capacity(struct ring_buffer *buf);

/*!
 * Resets the ring buffer, all data inside the buffer is lost
 */
void ring_buffer_reset(struct ring_buffer *buf);

/*!
 * Writes data of size len into the ring buffer buf.  Returns -1 if the
 * write failed or the number of bytes written
 */
int ring_buffer_write(struct ring_buffer *buf, const void *data,
			unsigned int len);

/*!
 * Advances the write counter by len, this is meant to be used with
 * the ring_buffer_write_ptr function.  Returns the number of bytes
 * actually advanced (the capacity of the buffer)
 */
int ring_buffer_write_advance(struct ring_buffer *buf, unsigned int len);

/*!
 * Returns the write pointer with write offset specified by offset.  Careful
 * not to write past the end of the buffer.  Use the ring_buffer_avail_no_wrap
 * function, and ring_buffer_write_advance.
 */
unsigned char *ring_buffer_write_ptr(struct ring_buffer *buf,
					unsigned int offset);

/*!
 * Returns the number of free bytes available in the buffer
 */
int ring_buffer_avail(struct ring_buffer *buf);

/*!
 * Returns the number of free bytes available in the buffer without wrapping
 */
int ring_buffer_avail_no_wrap(struct ring_buffer *buf);

/*!
 * Reads data from the ring buffer buf into memory region pointed to by data.
 * A maximum of len bytes will be read.  Returns -1 if the read failed or
 * the number of bytes read
 */
int ring_buffer_read(struct ring_buffer *buf, void *data,
			unsigned int len);

/*!
 * Returns the read pointer with read offset specified by offset.  No bounds
 * checking is performed.  Be careful not to read past the end of the buffer.
 * Use the ring_buffer_len_no_wrap function, and ring_buffer_drain.
 */
unsigned char *ring_buffer_read_ptr(struct ring_buffer *buf,
					unsigned int offset);

/*!
 * Returns the number of bytes currently available to be read in the buffer
 */
int ring_buffer_len(struct ring_buffer *buf);

/*!
 * Returns the number of bytes currently available to be read in the buffer
 * without wrapping.
 */
int ring_buffer_len_no_wrap(struct ring_buffer *buf);

/*!
 * Drains the ring buffer of len bytes.  Returns the number of bytes the
 * read counter was actually advanced.
 */
int ring_buffer_drain(struct ring_buffer *buf, unsigned int len);
