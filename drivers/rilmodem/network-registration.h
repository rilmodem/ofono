#ifndef __OFONO_NETWORK_REG_H
#define __OFONO_NETWORK_REG_H

struct netreg_data {
	GRil *ril;
	char mcc[OFONO_MAX_MCC_LENGTH + 1];
	char mnc[OFONO_MAX_MNC_LENGTH + 1];
	int signal_index; /* If strength is reported via CIND */
	int signal_min; /* min strength reported via CIND */
	int signal_max; /* max strength reported via CIND */
	int signal_invalid; /* invalid strength reported via CIND */
	int tech;
	struct ofono_network_time time;
	guint nitz_timeout;
	unsigned int vendor;
};

void ril_registration_status(struct ofono_netreg *netreg,
					ofono_netreg_status_cb_t cb,
					void *data);


void ril_current_operator(struct ofono_netreg *netreg,
				ofono_netreg_operator_cb_t cb, void *data);
				
void ril_list_operators(struct ofono_netreg *netreg,
				ofono_netreg_operator_list_cb_t cb, void *data);
				
void ril_netreg_remove(struct ofono_netreg *netreg);

void ril_register_auto(struct ofono_netreg *netreg,
				ofono_netreg_register_cb_t cb, void *data);

void ril_register_manual(struct ofono_netreg *netreg,
				const char *mcc, const char *mnc,
				ofono_netreg_register_cb_t cb, void *data);
				
void ril_signal_strength(struct ofono_netreg *netreg,
				ofono_netreg_strength_cb_t cb, void *data);
				
void ril_nitz_notify(struct ril_msg *message, gpointer user_data);

void ril_strength_notify(struct ril_msg *message, gpointer user_data);

void ril_network_state_change(struct ril_msg *message,
							gpointer user_data);

#endif
