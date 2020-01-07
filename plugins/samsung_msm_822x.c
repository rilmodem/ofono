#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define OFONO_API_SUBJECT_TO_CHANGE

#include <stddef.h>

#include <ofono/plugin.h>
#include <ofono/log.h>
#include <ofono/modem.h>

#include "ofono.h"

#include "drivers/rilmodem/rilmodem.h"
#include "drivers/rilmodem/vendor.h"
#include "gril.h"
#include "ril.h"

static const char *samsung_msm_822x_get_driver_type(enum ofono_atom_type atom)
{
    switch (atom) {
    default:
        return RILMODEM;
    }
}

static const char *samsung_msm_822x_request_id_to_string(int req)
{
    switch (req) {
    default:
        return NULL;
    }
}

static int samsung_msm_822x_probe(struct ofono_modem *modem)
{
    return ril_create(modem, OFONO_RIL_VENDOR_SAMSUNG_MSM_822x,
                      samsung_msm_822x_request_id_to_string,
                      NULL,
                      samsung_msm_822x_get_driver_type);
}

static struct ofono_modem_driver samsung_msm_822x_driver = {
    .name = "samsung_msm_822x",
    .probe = samsung_msm_822x_probe,
    .remove = ril_remove,
    .enable = ril_enable,
    .disable = ril_disable,
    .pre_sim = ril_pre_sim,
    .post_sim = ril_post_sim,
    .post_online = ril_post_online,
    .set_online = ril_set_online,
};

/*
 * This plugin is a device plugin for Samsung's devices with MSM-822x baseband that use
 * RIL interface. The plugin 'rildev' is used to determine which RIL plugin
 * should be loaded based upon an environment variable.
 */
static int samsung_msm_822x_init(void)
{
    int retval = ofono_modem_driver_register(&samsung_msm_822x_driver);

    if (retval)
        DBG("ofono_modem_driver_register returned: %d", retval);

    return retval;
}

static void samsung_msm_822x_exit(void)
{
    DBG("");
    ofono_modem_driver_unregister(&samsung_msm_822x_driver);
}

OFONO_PLUGIN_DEFINE(samsung_msm_822x, "Modem driver for Samsung devices based on MSM-822x baseband",
                    VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT, samsung_msm_822x_init, samsung_msm_822x_exit)