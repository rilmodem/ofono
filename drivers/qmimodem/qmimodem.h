/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011-2012  Intel Corporation. All rights reserved.
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

#include "util.h"

extern void qmi_devinfo_init(void);
extern void qmi_devinfo_exit(void);

extern void qmi_netreg_init(void);
extern void qmi_netreg_exit(void);

extern void qmi_voicecall_init(void);
extern void qmi_voicecall_exit(void);

extern void qmi_sim_legacy_init(void);
extern void qmi_sim_legacy_exit(void);

extern void qmi_sim_init(void);
extern void qmi_sim_exit(void);

extern void qmi_sms_init(void);
extern void qmi_sms_exit(void);

extern void qmi_ussd_init(void);
extern void qmi_ussd_exit(void);

extern void qmi_gprs_init(void);
extern void qmi_gprs_exit(void);

extern void qmi_gprs_context_init(void);
extern void qmi_gprs_context_exit(void);

extern void qmi_radio_settings_init(void);
extern void qmi_radio_settings_exit(void);

extern void qmi_location_reporting_init(void);
extern void qmi_location_reporting_exit(void);
