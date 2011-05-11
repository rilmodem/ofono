/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2010  Nokia Corporation and/or its subsidiary(-ies).
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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <gisi/netlink.h>
#include <glib.h>

#include <ofono/log.h>

#include <drivers/isimodem/debug.h>
#include "nokia-gpio.h"

#define GPIO_SWITCH	"/sys/devices/platform/gpio-switch"
#define DEV_CMT		"/dev/cmt"

enum rapu_type {
	RAPU_TYPE_1,
	RAPU_TYPE_2,
};

enum retry_count {
	RETRY_COUNT_RESET = 5,
	RETRY_COUNT_POWER_ON = 10,
};

enum phonet_link {
	PHONET_LINK_NONE = 0,
	PHONET_LINK_DOWN,
	PHONET_LINK_UP,
};

enum power_event {
	POWER_EVENT_PHONET_LINK_UP = 1,
	POWER_EVENT_PHONET_LINK_DOWN,
	POWER_EVENT_ON,
	POWER_EVENT_ON_TIMEOUT,
	POWER_EVENT_REBOOT_TIMEOUT,
	POWER_EVENT_OFF,
	POWER_EVENT_OFF_IMMEDIATELY,
	POWER_EVENT_OFF_TIMEOUT,
	POWER_EVENT_OFF_COMPLETE,
};

struct gpio_data {
	GIsiPhonetNetlink *link;
	gpio_finished_cb_t callback;
	void *data;

	enum power_state state;
	enum phonet_link current;
	enum phonet_link target;
	enum power_event timer_event;
	enum rapu_type rapu;

	guint timeout_source;

	unsigned retry_count;
	unsigned have_gpio_switch:1;
	unsigned have_cmt_en:1;
	unsigned have_cmt_rst_rq:1;
	unsigned have_cmt_rst:1;
	unsigned have_cmt_bsi:1;
	unsigned have_cmt_apeslpx:1;
	unsigned reset_in_progress:1;
	unsigned startup_in_progress:1;
};

static struct gpio_data self;

#define _(X) case X: return #X

static inline char const *gpio_power_event_name(enum power_event value)
{
	switch (value) {
		_(POWER_EVENT_PHONET_LINK_UP);
		_(POWER_EVENT_PHONET_LINK_DOWN);
		_(POWER_EVENT_ON);
		_(POWER_EVENT_ON_TIMEOUT);
		_(POWER_EVENT_REBOOT_TIMEOUT);
		_(POWER_EVENT_OFF);
		_(POWER_EVENT_OFF_IMMEDIATELY);
		_(POWER_EVENT_OFF_TIMEOUT);
		_(POWER_EVENT_OFF_COMPLETE);
	}
	return "<UNKNOWN>";
}

char const *gpio_power_state_name(enum power_state value)
{
	switch (value) {
		_(POWER_STATE_NONE);
		_(POWER_STATE_ON_STARTED);
		_(POWER_STATE_ON);
		_(POWER_STATE_ON_RESET);
		_(POWER_STATE_ON_FAILED);
		_(POWER_STATE_OFF_STARTED);
		_(POWER_STATE_OFF_WAITING);
		_(POWER_STATE_OFF);
	}
	return "<UNKNOWN>";
}

#undef _

static void gpio_power_state_machine(enum power_event event);
static void gpio_power_set_state(enum power_state new_state);

static int file_exists(char const *filename)
{
	struct stat st;

	return stat(filename, &st) == 0;
}

static int dir_exists(char const *filename)
{
	struct stat st;

	return stat(filename, &st) == 0 && S_ISDIR(st.st_mode);
}

static int file_write(char const *filename, char const *output)
{
	FILE *f;

	f = fopen(filename, "r+");
	if (f == NULL) {
		DBG("%s: %s (%d)", filename, strerror(errno), errno);
		return -1;
	}

	fputs(output, f);

	return fclose(f);
}

static int gpio_write(char *line, int value)
{
	char filename[256];

	DBG("(\"%s\", \"%s\")", line, value ? "active" : "inactive");

	if (self.have_gpio_switch) {
		snprintf(filename, sizeof filename, "%s/%s/%s",
				GPIO_SWITCH, line, "state");
		return file_write(filename, value ? "active" : "inactive");
	} else {
		snprintf(filename, sizeof filename, "%s/%s/%s",
				DEV_CMT, line, "value");
		return file_write(filename, value ? "1" : "0");
	}
}

#define GPIO_WRITE(line, value) \
	(self.have_ ## line ? gpio_write(#line, value) : 0)

static int gpio_line_probe(char const *line)
{
	char filename[256];
	int result;

	if (self.have_gpio_switch)
		snprintf(filename, sizeof filename,
				"%s/%s/state", GPIO_SWITCH, line);
	else
		snprintf(filename, sizeof filename,
				"%s/%s/value", DEV_CMT, line);

	result = file_exists(filename);

	DBG("%s: %s", line, result ? "found" : "not found");

	return result;
}

/*
 * Modem start up function
 *
 * Sets all lines down and leaves "power key" pressed (power key must
 * be released after some time)
 */
static void gpio_start_modem_power_on(void)
{
	DBG("");

	if (self.startup_in_progress)
		return;
	self.startup_in_progress = 1;

	GPIO_WRITE(cmt_apeslpx, 0);	/* skip flash mode */
	GPIO_WRITE(cmt_rst_rq, 0);	/* prevent current drain */

	switch (self.rapu) {
	case RAPU_TYPE_2:
		GPIO_WRITE(cmt_en, 0);
		/* 15 ms needed for ASIC poweroff */
		usleep(20000);
		GPIO_WRITE(cmt_en, 1);
		break;

	case RAPU_TYPE_1:
		GPIO_WRITE(cmt_en, 0);
		GPIO_WRITE(cmt_bsi, 0);	/* toggle BSI visible to modem */
		GPIO_WRITE(cmt_rst, 0);	/* Assert PURX */
		GPIO_WRITE(cmt_en, 1);	/* Press "power key" */
		GPIO_WRITE(cmt_rst, 1);	/* Release CMT to boot */
		break;
	}

	GPIO_WRITE(cmt_rst_rq, 1);
}

static void gpio_finish_modem_power_on(void)
{
	DBG("");

	if (!self.startup_in_progress)
		return;

	self.startup_in_progress = 0;

	switch (self.rapu) {
	case RAPU_TYPE_2:
		break;

	case RAPU_TYPE_1:
		GPIO_WRITE(cmt_en, 0);	/* release "power key" */
		break;
	}
}

static void gpio_start_modem_reset(void)
{
	DBG("");

	if (self.reset_in_progress)
		return;
	self.reset_in_progress = 1;

	if (self.have_cmt_rst_rq) {
		GPIO_WRITE(cmt_rst_rq, 0); /* Just in case */
		GPIO_WRITE(cmt_rst_rq, 1);
	} else {
		gpio_start_modem_power_on();
	}
}

static void gpio_finish_modem_reset(void)
{
	DBG("");

	if (!self.reset_in_progress)
		return;

	self.reset_in_progress = 0;
	gpio_finish_modem_power_on();
}

static void gpio_finish_modem_power_off(void)
{
	DBG("");

	if (self.reset_in_progress)
		gpio_finish_modem_reset();

	if (self.startup_in_progress)
		gpio_finish_modem_power_on();

	GPIO_WRITE(cmt_apeslpx, 0);	/* skip flash mode */
	GPIO_WRITE(cmt_rst_rq, 0);	/* prevent current drain */

	switch (self.rapu) {
	case RAPU_TYPE_2:
		GPIO_WRITE(cmt_en, 0);	/* Power off */
		break;

	case RAPU_TYPE_1:
		GPIO_WRITE(cmt_en, 0);	/* release "power key" */
		GPIO_WRITE(cmt_rst, 0);	/* force modem to reset state */
		GPIO_WRITE(cmt_rst, 1);	/* release modem to be powered
							off by bootloader */
		break;
	}
}

static gboolean gpio_power_timer_cb(gpointer user)
{
	self.timeout_source = 0;

	if (self.timer_event)
		gpio_power_state_machine(self.timer_event);

	return FALSE;
}


static void gpio_power_state_machine(enum power_event event)
{
	enum power_state new_state;

	DBG("(%s) @ state %s",
		gpio_power_event_name(event),
		gpio_power_state_name(self.state));

	switch (event) {
	case POWER_EVENT_ON:
		self.target = PHONET_LINK_UP;

		if (self.current == PHONET_LINK_NONE)
			return;

		switch (self.state) {
		case POWER_STATE_ON_STARTED:
		case POWER_STATE_ON_RESET:
		case POWER_STATE_ON:
			/* Do nothing */
			break;

		case POWER_STATE_OFF_STARTED:
			/* Do nothing */
			break;

		case POWER_STATE_NONE:
		case POWER_STATE_OFF_WAITING:
		case POWER_STATE_OFF:
		case POWER_STATE_ON_FAILED:
			gpio_power_set_state(POWER_STATE_ON_STARTED);
			break;
		}
		return;

	case POWER_EVENT_PHONET_LINK_DOWN:

		switch (self.target) {
		case PHONET_LINK_UP:
			break;

		case PHONET_LINK_DOWN:
		case PHONET_LINK_NONE:
		default:
			if (self.state == POWER_STATE_OFF ||
					self.state == POWER_STATE_NONE)
				new_state = POWER_STATE_OFF;
			else
				new_state = POWER_STATE_OFF_WAITING;

			gpio_power_set_state(new_state);
			return;
		}

		switch (self.state) {
		case POWER_STATE_NONE:
			/* first connection down event => start modem */
			gpio_power_set_state(POWER_STATE_ON_STARTED);
			break;

		case POWER_STATE_ON_STARTED:
		case POWER_STATE_ON_RESET:
			break;

		default:
			self.retry_count = 0;
			gpio_power_set_state(POWER_STATE_ON_RESET);
			break;
		}
		return;

	case POWER_EVENT_ON_TIMEOUT:

		if (self.target == PHONET_LINK_DOWN)
			new_state = POWER_STATE_OFF_STARTED;
		else if (self.retry_count <= RETRY_COUNT_POWER_ON)
			new_state = POWER_STATE_ON_STARTED;
		else
			new_state = POWER_STATE_ON_FAILED;

		gpio_power_set_state(new_state);
		return;

	case POWER_EVENT_REBOOT_TIMEOUT:
		/* Modem not rebooting - try to powercycle */
		if (self.target == PHONET_LINK_DOWN)
			new_state = POWER_STATE_OFF_STARTED;
		else if (self.retry_count <= RETRY_COUNT_RESET)
			new_state = POWER_STATE_ON_RESET;
		else
			new_state = POWER_STATE_ON_STARTED;

		gpio_power_set_state(new_state);
		return;

	case POWER_EVENT_PHONET_LINK_UP:

		switch (self.state) {
		case POWER_STATE_NONE:
			return;

		case POWER_STATE_ON_STARTED:
		case POWER_STATE_ON_RESET:
			break;

		case POWER_STATE_ON:
			return;

		case POWER_STATE_OFF_STARTED:
		case POWER_STATE_OFF_WAITING:
		case POWER_STATE_OFF:
		case POWER_STATE_ON_FAILED:
			DBG("LINK_UP event while modem should be powered off");
			/* should never come here */
			break;
		}

		if (self.target == PHONET_LINK_DOWN)
			gpio_power_set_state(POWER_STATE_OFF_STARTED);
		else
			gpio_power_set_state(POWER_STATE_ON);
		return;

	case POWER_EVENT_OFF:
		self.target = PHONET_LINK_DOWN;

		switch (self.state) {
		case POWER_STATE_ON_STARTED:
		case POWER_STATE_ON_RESET:
			/* Do nothing until a timer expires */
			break;

		case POWER_STATE_ON:
			gpio_power_set_state(POWER_STATE_OFF_STARTED);
			break;

		case POWER_STATE_OFF_STARTED:
		case POWER_STATE_OFF_WAITING:
		case POWER_STATE_OFF:
			/* Do nothing */
			break;

		case POWER_STATE_NONE:
		case POWER_STATE_ON_FAILED:
			gpio_power_set_state(POWER_STATE_OFF);
			break;
		}
		return;

	case POWER_EVENT_OFF_IMMEDIATELY:
		gpio_power_set_state(POWER_STATE_OFF);
		return;

	case POWER_EVENT_OFF_TIMEOUT:
		DBG("Modem power off timed out");
		gpio_power_set_state(POWER_STATE_OFF);
		return;

	case POWER_EVENT_OFF_COMPLETE:
		if (self.state == POWER_STATE_OFF_WAITING) {
			DBG("Modem shutdown complete");
			gpio_power_set_state(POWER_STATE_OFF);
		}
		return;
	}

	DBG("Event %s (%d) not handled", gpio_power_event_name(event), event);
}


static void gpio_power_set_state(enum power_state new_state)
{
	enum power_state old_state = self.state;
	unsigned timeout = 0;
	enum power_event timer_event;

	DBG("(%s) at (%s)%s",
		gpio_power_state_name(new_state),
		gpio_power_state_name(old_state),
		new_state == old_state ? " - already" : "");

	switch (old_state) {
	case POWER_STATE_ON_STARTED:
		gpio_finish_modem_power_on();
		break;

	case POWER_STATE_ON_RESET:
		gpio_finish_modem_reset();
		break;

	default:
		break;
	}

	if (self.timeout_source) {
		g_source_remove(self.timeout_source);
		self.timeout_source = 0;
		self.timer_event = 0;
	}

	if (old_state == new_state
			&& new_state != POWER_STATE_ON_STARTED
			&& new_state != POWER_STATE_ON_RESET)
		return;

	self.state = new_state;

	switch (self.state) {
	case POWER_STATE_NONE:
		break;

	case POWER_STATE_ON_STARTED:
		self.retry_count++;

		/* Maximum time modem power on procedure on can take */
		timeout = 5000;
		timer_event = POWER_EVENT_ON_TIMEOUT;
		gpio_start_modem_power_on();
		break;

	case POWER_STATE_ON_RESET:
		DBG("Starting modem restart timeout");

		/* Time allowed for modem to restart after crash */
		timeout = 5000;
		timer_event = POWER_EVENT_REBOOT_TIMEOUT;

		if (self.retry_count++ > 0)
			gpio_start_modem_reset();
		break;

	case POWER_STATE_ON:
		DBG("Power on");
		self.retry_count = 0;
		break;

	case POWER_STATE_OFF_STARTED:
		DBG("Starting power off");

		/* Maximum time modem power_off can take */
		timeout = 6150;
		timer_event = POWER_EVENT_OFF_TIMEOUT;
		break;

	case POWER_STATE_OFF_WAITING:
		gpio_finish_modem_power_off();
		DBG("Waiting for modem to settle down");

		/* Cooling time after power off */
		timeout = 1000;
		timer_event = POWER_EVENT_OFF_COMPLETE;
		break;

	case POWER_STATE_OFF:

		if (old_state != POWER_STATE_OFF_WAITING
				&& old_state != POWER_STATE_ON_FAILED)
			gpio_finish_modem_power_off();
		break;

	case POWER_STATE_ON_FAILED:
		DBG("Link to modem cannot be established, giving up");
		gpio_finish_modem_power_off();
		break;
	}

	if (timeout) {
		self.timer_event = timer_event;
		self.timeout_source = g_timeout_add(timeout,
					gpio_power_timer_cb, NULL);
	}

	self.callback(new_state, self.data);
}

static void phonet_status_cb(GIsiModem *idx, enum GIsiPhonetLinkState state,
				char const *ifname, void *dummy)
{
	DBG("Link %s (%u) is %s",
		ifname, g_isi_modem_index(idx),
		state == PN_LINK_REMOVED ? "removed" :
		state == PN_LINK_DOWN ? "down" : "up");

	if (state == PN_LINK_UP) {

		if (self.current == PHONET_LINK_UP)
			return;

		self.current = PHONET_LINK_UP;

		/* link is up - we can lower cmt_rst_rq */
		GPIO_WRITE(cmt_rst_rq, 0);

		gpio_power_state_machine(POWER_EVENT_PHONET_LINK_UP);
	} else {

		if (self.current == PHONET_LINK_DOWN)
			return;

		self.current = PHONET_LINK_DOWN;

		gpio_power_state_machine(POWER_EVENT_PHONET_LINK_DOWN);
	}
}

static int gpio_probe_links(void)
{
	char const *gpiodir = "/sys/class/gpio";
	char const *cmtdir = "/dev/cmt";
	DIR *gpio;
	struct dirent *d, entry[1];

	if (file_exists(cmtdir)) {
		DBG("Using %s", cmtdir);
		return 0;
	}

	DBG("Using %s: trying to make links to %s", gpiodir, cmtdir);

	if (!dir_exists(cmtdir)) {
		if (mkdir(cmtdir, 0755) == -1) {
			DBG("%s: %s", cmtdir, strerror(errno));
			return -(errno = ENODEV);
		}
	}

	gpio = opendir(gpiodir);
	if (gpio == NULL) {
		DBG("%s: %s", "gpiodir", strerror(errno));
		return -(errno = ENODEV);
	}

	while (readdir_r(gpio, entry, &d) == 0) {
		char nn[PATH_MAX], name[PATH_MAX], from[PATH_MAX], to[PATH_MAX];
		FILE *nf;
		size_t len;

		if (d == NULL) {
			(void) closedir(gpio);
			return 0;
		}

		snprintf(nn, sizeof nn, "%s/%s/name", gpiodir, d->d_name);

		nf = fopen(nn, "rb");
		if (nf == NULL) {
			DBG("%s: %s", nn, strerror(errno));
			continue;
		}

		len = fread(name, sizeof name, 1, nf);

		if (ferror(nf)) {
			DBG("read from %s: %s", nn, strerror(errno));
			fclose(nf);
			continue;
		}

		fclose(nf);

		if (len < 4)
			continue;

		name[--len] = '\0';

		if (strncmp(name, "cmt_", 4))
			continue;

		snprintf(from, sizeof from, "%s/%s", gpiodir, d->d_name);
		snprintf(to, sizeof to, "%s/%s", cmtdir, name);

		if (symlink(from, to) == -1)
			DBG("%s: %s", to, strerror(errno));
	}

	DBG("%s: %s", "/sys/class/gpio", strerror(errno));

	(void) closedir(gpio);

	return -(errno = ENODEV);
}


int gpio_probe(GIsiModem *idx, unsigned addr, gpio_finished_cb_t cb, void *data)
{
	int error;

	if (cb == NULL) {
		DBG("gpio: No callback given");
		return -(errno = EFAULT);
	}

	if (self.callback) {
		DBG("gpio: %s", strerror(EBUSY));
		return -(errno = EBUSY);
	}

	if (g_isi_pn_netlink_by_modem(idx)) {
		DBG("Phonet link %p: %s", idx, strerror(EBUSY));
		return -(errno = EBUSY);
	}

	self.target = PHONET_LINK_NONE;
	self.have_gpio_switch = file_exists(GPIO_SWITCH);

	if (self.have_gpio_switch) {
		DBG("Using GPIO switch");
	} else {
		error = gpio_probe_links();
		if (error)
			return error;
	}

	/* GPIO lines availability depends on HW and SW versions */
	self.have_cmt_en = gpio_line_probe("cmt_en");
	self.have_cmt_rst_rq = gpio_line_probe("cmt_rst_rq");
	self.have_cmt_rst = gpio_line_probe("cmt_rst");
	self.have_cmt_bsi = gpio_line_probe("cmt_bsi");
	self.have_cmt_apeslpx = gpio_line_probe("cmt_apeslpx");

	if (!self.have_cmt_en) {
		DBG("Modem control GPIO lines are not available");
		memset(&self, 0, sizeof self);
		return -(errno = ENODEV);
	}

	if (self.have_cmt_bsi)
		self.rapu = RAPU_TYPE_1;
	else
		self.rapu = RAPU_TYPE_2;

	self.link = g_isi_pn_netlink_start(idx, phonet_status_cb, NULL);
	if (self.link == NULL) {
		memset(&self, 0, sizeof self);
		return -errno;
	}

	self.callback = cb;
	self.data = data;

	if (addr) {
		error = g_isi_pn_netlink_set_address(idx, addr);
		if (error && error != -EEXIST)
			DBG("g_isi_netlink_set_address: %s", strerror(-error));
	}

	return 0;
}

int gpio_remove(void *data)
{
	if (self.data != data)
		return -EINVAL;

	if (self.link)
		g_isi_pn_netlink_stop(self.link);

	if (self.timeout_source) {
		g_source_remove(self.timeout_source);
		self.timeout_source = 0;
	}

	memset(&self, 0, sizeof self);

	return 0;
}

int gpio_enable(void *data)
{
	if (self.data != data)
		return -EINVAL;

	if (self.state == POWER_STATE_ON)
		return 0;

	gpio_power_state_machine(POWER_EVENT_ON);

	return -EINPROGRESS;
}

int gpio_disable(void *data)
{
	if (self.data != data)
		return -EINVAL;

	if (self.state == POWER_STATE_OFF
			|| self.state == POWER_STATE_ON_FAILED)
		return 0;

	gpio_power_state_machine(POWER_EVENT_OFF);

	return -EINPROGRESS;
}
