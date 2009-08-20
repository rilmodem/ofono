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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <glib.h>

#include <ofono/types.h>

#include "common.h"

static const char *invalid_strings[] = {
	"33",
	"*",
	"**",
	"***",
	"*****",
	"******",
	"#",
	"##",
	"###",
	"####",
	"#####",
	"*#",
	"**#",
	"****#",
	"*****#",
	"**#",
	"*#",
	"##",
	"*04*98*0000*00000*00000#",
	NULL
};

static void test_invalid()
{
	char *sc;
	char *sia;
	char *sib;
	char *sic;
	char *sid;
	char *dn;
	int type;

	char *str;
	int i;
	gboolean ret;

	for (i = 0; invalid_strings[i]; i++) {
		if (g_test_verbose())
			g_print("%s...\n", invalid_strings[i]);

		str = strdup(invalid_strings[i]);

		ret = parse_ss_control_string(str, &type, &sc,
						&sia, &sib, &sic, &sid, &dn);
		if (strlen(sid))
			ret = FALSE;

		g_assert(ret == FALSE);

		free(str);
	}
}

static const char *valid_strings[] = {
	"*31#",
	"*31#+55555",
	"#31#",
	"#31#+55555",
	"*21*+55555*10*20#",
	"*21*+55555*10#",
	"*21**20#",
	"*21*+55555#",
	"*21**10*20#",
	"*21**10#",
	"*21***20#",
	"*21#",
	"**21#",
	"*#21#",
	"#21#",
	"##21#",
	NULL
};

static void test_valid()
{
	char *sc;
	char *sia;
	char *sib;
	char *sic;
	char *sid;
	char *dn;
	int type;
	gboolean ret;

	char *str;
	int i;

	for (i = 0; valid_strings[i]; i++) {
		if (g_test_verbose())
			g_print("%s...", valid_strings[i]);

		str = strdup(valid_strings[i]);

		ret = parse_ss_control_string(str, &type, &sc,
						&sia, &sib, &sic, &sid, &dn);
		if (strlen(sid))
			ret = FALSE;

		g_assert(ret == TRUE);

		if (g_test_verbose())
			g_print("parsed as: %d, %s, %s, %s, %s, %s\n",
					type, sc, sia, sib, sic, dn);

		free(str);
	}
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/testutil/Invalid", test_invalid);
	g_test_add_func("/testutil/Valid", test_valid);

	return g_test_run();
}
