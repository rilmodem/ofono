/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2009-2010 Nokia Corporation and/or its subsidiary(-ies).
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

extern struct ofono_modem *isi_modem_by_interface(char const *ifname);

extern void isi_phonebook_init();
extern void isi_phonebook_exit();

extern void isi_devinfo_init();
extern void isi_devinfo_exit();

extern void isi_netreg_init();
extern void isi_netreg_exit();

extern void isi_voicecall_init();
extern void isi_voicecall_exit();

extern void isi_sms_init();
extern void isi_sms_exit();

extern void isi_cbs_init();
extern void isi_cbs_exit();

extern void isi_sim_init();
extern void isi_sim_exit();

extern void isi_ussd_init();
extern void isi_ussd_exit();

extern void isi_ssn_init();
extern void isi_ssn_exit();

extern void isi_call_forwarding_init();
extern void isi_call_forwarding_exit();

extern void isi_call_settings_init();
extern void isi_call_settings_exit();

extern void isi_call_barring_init();
extern void isi_call_barring_exit();

extern void isi_call_meter_init();
extern void isi_call_meter_exit();

extern void isi_radio_settings_init();
extern void isi_radio_settings_exit();

extern void isi_gprs_init();
extern void isi_gprs_exit();

extern void isi_gprs_context_init();
extern void isi_gprs_context_exit();

extern void isi_audio_settings_init();
extern void isi_audio_settings_exit();
