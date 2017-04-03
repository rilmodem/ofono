#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <glib.h>

#include <ofono/log.h>
#include <ofono/modem.h>
#include <ofono/netreg.h>
#include <ofono/spn-table.h>

#include "common.h"
#include "gril.h"

#include "grilreply.h"
#include "grilrequest.h"
#include "grilunsol.h"
#include <ofono/netreg.h>
#include "drivers/rilmodem/network-registration.h"
#include "mtk2_constants.h"
#include "mtk2modem.h"
#include "drivers/mtkmodem/mtkunsol.h"
#include "drivers/mtkmodem/mtkrequest.h"
#include "src/ofono.h"
#include "plugins/ril.h"

struct ril_data {
	GRil *ril;
	enum ofono_ril_vendor vendor;
	int sim_status_retries;
	ofono_bool_t init_state;
	ofono_bool_t ofono_online;
	int radio_state;
	struct ofono_sim *sim;
	int rild_connect_retries;
	GRilMsgIdToStrFunc request_id_to_string;
	GRilMsgIdToStrFunc unsol_request_to_string;
	ril_get_driver_type_func get_driver_type;
	struct cb_data *set_online_cbd;
};

static void mtk2_reg_suspended_cb(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *md = ofono_modem_get_data(modem);

	if (message->error != RIL_E_SUCCESS) {
		ofono_error("%s: RIL error %s", __func__,
				ril_error_to_string(message->error));
		return;
	}

	g_ril_print_response_no_args(md->ril, message);
}

static void mtk2_reg_suspended(struct ril_msg *message, gpointer user_data)
{
	struct ofono_modem *modem = user_data;
	struct ril_data *md = ofono_modem_get_data(modem);
	struct parcel rilp;
	int session_id;

	session_id = g_mtk_unsol_parse_registration_suspended(md->ril, message);
	if (session_id < 0) {
		ofono_error("%s: parse error", __func__);
		return;
	}

	g_mtk_request_resume_registration(md->ril, session_id, &rilp);

	if (g_ril_send(md->ril, MTK2_RIL_REQUEST_RESUME_REGISTRATION, &rilp,
			mtk2_reg_suspended_cb, modem, NULL) == 0)
		ofono_error("%s: failure sending request", __func__);
}

static gboolean mtk2_delayed_register(gpointer user_data)
{
	struct ofono_netreg *netreg = user_data;
	struct netreg_data *nd = ofono_netreg_get_data(netreg);
				
	g_ril_register(nd->ril, MTK2_RIL_UNSOL_RESPONSE_REGISTRATION_SUSPENDED,
			mtk2_reg_suspended, netreg);

	/* This makes the timeout a single-shot */
	return FALSE;
}

static int mtk2_netreg_probe(struct ofono_netreg *netreg, unsigned int vendor,
				void *data)
{
	GRil *ril = data;
	struct netreg_data *nd;

	nd = g_new0(struct netreg_data, 1);

	nd->ril = g_ril_clone(ril);
	nd->vendor = vendor;
	nd->tech = RADIO_TECH_UNKNOWN;
	nd->time.sec = -1;
	nd->time.min = -1;
	nd->time.hour = -1;
	nd->time.mday = -1;
	nd->time.mon = -1;
	nd->time.year = -1;
	nd->time.dst = 0;
	nd->time.utcoff = 0;
	ofono_netreg_set_data(netreg, nd);

	/*
	 * ofono_netreg_register() needs to be called after
	 * the driver has been set in ofono_netreg_create(),
	 * which calls this function.  Most other drivers make
	 * some kind of capabilities query to the modem, and then
	 * call register in the callback; we use the idle loop here.
	 */
	g_idle_add(mtk2_delayed_register, netreg);

	return 0;
}

static struct ofono_netreg_driver driver = {
	.name				= MTK2MODEM,
	.probe				= mtk2_netreg_probe,
	.remove				= ril_netreg_remove,
	.registration_status		= ril_registration_status,
	.current_operator		= ril_current_operator,
	.list_operators			= ril_list_operators,
	.register_auto			= ril_register_auto,
	.register_manual		= ril_register_manual,
	.strength			= ril_signal_strength,
};

void mtk2_netreg_init(void)
{
	ofono_netreg_driver_register(&driver);
}

void mtk2_netreg_exit(void)
{
	ofono_netreg_driver_unregister(&driver);
}
