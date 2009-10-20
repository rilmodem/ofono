/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2008-2009  Intel Corporation. All rights reserved.
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

#ifndef __OFONO_TYPES_H
#define __OFONO_TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef	FALSE
#define	FALSE	(0)
#endif

#ifndef	TRUE
#define	TRUE	(!FALSE)
#endif

typedef int		ofono_bool_t;

/* MCC is always three digits. MNC is either two or three digits */
#define OFONO_MAX_MCC_LENGTH 3
#define OFONO_MAX_MNC_LENGTH 3

typedef void (*ofono_destroy_func)(void *data);

/* 27.007 Section 6.2 */
enum ofono_clir_option {
	OFONO_CLIR_OPTION_DEFAULT = 0,
	OFONO_CLIR_OPTION_INVOCATION,
	OFONO_CLIR_OPTION_SUPPRESSION
};

/* 27.007 Section 6.2 */
enum ofono_cug_option {
	OFONO_CUG_OPTION_DEFAULT = 0,
	OFONO_CUG_OPTION_INVOCATION = 1,
};

enum ofono_error_type {
	OFONO_ERROR_TYPE_NO_ERROR = 0,
	OFONO_ERROR_TYPE_CME,
	OFONO_ERROR_TYPE_CMS,
	OFONO_ERROR_TYPE_CEER,
	OFONO_ERROR_TYPE_FAILURE
};

enum ofono_disconnect_reason {
	OFONO_DISCONNECT_REASON_UNKNOWN = 0,
	OFONO_DISCONNECT_REASON_LOCAL_HANGUP,
	OFONO_DISCONNECT_REASON_REMOTE_HANGUP,
	OFONO_DISCONNECT_REASON_ERROR,
};

struct ofono_error {
	enum ofono_error_type type;
	int error;
};

#define OFONO_MAX_PHONE_NUMBER_LENGTH 20

struct ofono_phone_number {
	char number[OFONO_MAX_PHONE_NUMBER_LENGTH + 1];
	int type;
};

struct ofono_call {
	unsigned id;
	int type;
	int direction;
	int status;
	struct ofono_phone_number phone_number;
	int clip_validity;
};

#ifdef __cplusplus
}
#endif

#endif /* __OFONO_TYPES_H */
