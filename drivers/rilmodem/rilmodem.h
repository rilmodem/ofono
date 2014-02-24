/*
 *
 *  oFono - Open Source Telephony - RIL Modem Support
 *
 *  Copyright (C) 2008-2011  Intel Corporation. All rights reserved.
 *  Copyright (C) 2012 Canonical Ltd.
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

#include "rilutil.h"

#define RILMODEM "rilmodem"

/* Shared constants */
#define EF_STATUS_INVALIDATED 0
#define EF_STATUS_VALID 1

extern void ril_devinfo_init(void);
extern void ril_devinfo_exit(void);

extern void ril_call_volume_init(void);
extern void ril_call_volume_exit(void);

extern void ril_voicecall_init(void);
extern void ril_voicecall_exit(void);

extern void ril_sim_init(void);
extern void ril_sim_exit(void);

extern void ril_sms_init(void);
extern void ril_sms_exit(void);

extern void ril_netreg_init(void);
extern void ril_netreg_exit(void);

extern void ril_gprs_init(void);
extern void ril_gprs_exit(void);

extern void ril_gprs_context_init(void);
extern void ril_gprs_context_exit(void);

extern void ril_ussd_init(void);
extern void ril_ussd_exit(void);

extern void ril_call_settings_init(void);
extern void ril_call_settings_exit(void);

extern void ril_call_forwarding_init(void);
extern void ril_call_forwarding_exit(void);
