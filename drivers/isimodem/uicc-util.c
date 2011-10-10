/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2011  ST-Ericsson AB.
 *  Copyright (C) 2011  Nokia Corporation and/or its subsidiary(-ies).
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/sim.h>

#include "simutil.h"
#include "sim.h"
#include "uicc-util.h"
#include "uicc.h"
#include "debug.h"

#define USIM_APP_DEDICATED_FILE		0x7FFF

gboolean uicc_get_fileid_path(struct uicc_sim_data *sd,
				int *mf_path,
				int *df1_path,
				int *df2_path,
				unsigned char *df_len,
				int fileid)
{
	switch (fileid) {
	case SIM_EFPL_FILEID:
	case SIM_EF_ICCID_FILEID:
		*mf_path = SIM_MF_FILEID;
		*df1_path = 0x0000;
		*df2_path = 0x0000;
		*df_len = 2;
		break;
	case SIM_EFSMSP_FILEID:
	case SIM_EFSDN_FILEID:
	case SIM_EFMSISDN_FILEID:
		*mf_path = SIM_MF_FILEID;

		if (sd->app_type == UICC_APPL_TYPE_ICC_SIM)
			*df1_path = SIM_DFTELECOM_FILEID;
		else
			*df1_path = USIM_APP_DEDICATED_FILE;

		*df2_path = 0x0000;
		*df_len = 4;
		break;
	case SIM_EFLI_FILEID:
	case SIM_EFSPN_FILEID:
	case SIM_EFAD_FILEID:
	case SIM_EFPNN_FILEID:
	case SIM_EFOPL_FILEID:
	case SIM_EFMBDN_FILEID:
	case SIM_EFMBI_FILEID:
	case SIM_EFMWIS_FILEID:
	case SIM_EFSPDI_FILEID:
	case SIM_EFECC_FILEID:
	case SIM_EFCBMI_FILEID:
	case SIM_EFCBMIR_FILEID:
	case SIM_EFCBMID_FILEID:
	case SIM_EFIMSI_FILEID:
	case SIM_EFPHASE_FILEID: /*Did not find in TS 31.102 v6.21.0*/
	case SIM_EFARR_FILEID:
	case SIM_EF_CPHS_INFORMATION_FILEID: /*Found from unofficial source*/
		*mf_path = SIM_MF_FILEID;

		if (sd->app_type == UICC_APPL_TYPE_ICC_SIM)
			*df1_path = SIM_DFGSM_FILEID;
		else
			*df1_path = USIM_APP_DEDICATED_FILE;

		*df2_path = 0x0000;
		*df_len = 4;
		break;
		/* No info */
	case SIM_EF_CPHS_MBDN_FILEID:
	case SIM_EF_CPHS_MWIS_FILEID:
		DBG("======== No path info for %04X", fileid);
		return FALSE;
	case SIM_EFADN_FILEID: /* Only for SIM */
	case SIM_EFEXT1_FILEID: /* Only for SIM */
		*mf_path = SIM_MF_FILEID;
		*df1_path = SIM_DFTELECOM_FILEID;
		*df2_path = 0x0000;
		*df_len = 4;
		break;
	default:
		*mf_path = SIM_MF_FILEID;
		*df1_path = SIM_DFTELECOM_FILEID;
		*df2_path = SIM_DFPHONEBOOK_FILEID;
		*df_len = 6;
		break;
	}

	return TRUE;
}

uint8_t uicc_get_sfi(const int fileid)
{
	/* SFI list from 3GPP TS 31.102 Annex H */
	switch (fileid) {
	case SIM_EFECC_FILEID:
		return 01;
	case SIM_EFLI_FILEID:
		return 02;
	case SIM_EFAD_FILEID:
		return 03;
	case SIM_EFIMSI_FILEID:
		return 07;
	case SIM_EFCBMID_FILEID:
		return 0x0E;
	case SIM_EFPNN_FILEID:
		return 0x19;
	case SIM_EFOPL_FILEID:
		return 0x1A;
	default:
		return UICC_SFI_NOT_PRESENT;
	}
}
