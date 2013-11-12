/*
 *
 *  oFono - Open Source Telephony
 *
 *  Copyright (C) 2013  Canonical Ltd.
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

#include <errno.h>
#include <string.h>
#include <stdlib.h>

#include <glib.h>

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/types.h>
#include <ofono/log.h>
#include <ofono/plugin.h>
#include <ofono/sim-mnclength.h>

struct mcc_mnclength {
	int mcc;
	int mnclength;
};

/*
 * Database of MCC to MNC length correspondences based on "Mobile Network Codes
 * (MNC) for the international identification plan for public networks and
 * subscriptions (According to Recommendation ITU-T E.212 (05/2008))". Based on
 * position on 1st January 2013 (http://www.itu.int/pub/T-SP-E.212B-2013).
 * Latest version of that document can be found in
 * http://www.itu.int/pub/T-SP-E.212B. Countries wiht no operators have been
 * given a default length depending on their geographical area.
 */
static struct mcc_mnclength mnclen_db[] = {
	{202, 2},	/* Greece */
	{204, 2},	/* Netherlands (Kingdom of the) */
	{206, 2},	/* Belgium */
	{208, 2},	/* France */
	{212, 2},	/* Monaco (Principality of) */
	{213, 2},	/* Andorra (Principality of) */
	{214, 2},	/* Spain */
	{216, 2},	/* Hungary */
	{218, 2},	/* Bosnia and Herzegovina */
	{219, 2},	/* Croatia (Republic of) */
	{220, 2},	/* Serbia (Republic of) */
	{222, 2},	/* Italy */
	{225, 2},	/* Vatican City State */
	{226, 2},	/* Romania */
	{228, 2},	/* Switzerland (Confederation of) */
	{230, 2},	/* Czech Republic */
	{231, 2},	/* Slovak Republic */
	{232, 2},	/* Austria */
	{234, 2},	/* United Kingdom of G. Britain and Northern Ireland */
	{235, 2},	/* United Kingdom of G. Britain and Northern Ireland */
	{238, 2},	/* Denmark */
	{240, 2},	/* Sweden */
	{242, 2},	/* Norway */
	{244, 2},	/* Finland */
	{246, 2},	/* Lithuania (Republic of) */
	{247, 2},	/* Latvia (Republic of) */
	{248, 2},	/* Estonia (Republic of) */
	{250, 2},	/* Russian Federation */
	{255, 2},	/* Ukraine */
	{257, 2},	/* Belarus (Republic of) */
	{259, 2},	/* Moldova (Republic of) */
	{260, 2},	/* Poland (Republic of) */
	{262, 2},	/* Germany (Federal Republic of) */
	{266, 2},	/* Gibraltar */
	{268, 2},	/* Portugal */
	{270, 2},	/* Luxembourg */
	{272, 2},	/* Ireland */
	{274, 2},	/* Iceland */
	{276, 2},	/* Albania (Republic of) */
	{278, 2},	/* Malta */
	{280, 2},	/* Cyprus (Republic of) */
	{282, 2},	/* Georgia */
	{283, 2},	/* Armenia (Republic of) */
	{284, 2},	/* Bulgaria (Republic of) */
	{286, 2},	/* Turkey */
	{288, 2},	/* Faroe Islands */
	{290, 2},	/* Greenland (Denmark) */
	{292, 2},	/* San Marino (Republic of) */
	{293, 2},	/* Slovenia (Republic of) */
	{294, 2},	/* The Former Yugoslav Republic of Macedonia */
	{295, 2},	/* Liechtenstein (Principality of) */
	{297, 2},	/* Montenegro (Republic of) */
	{302, 3},	/* Canada */
	{308, 2},	/* Saint Pierre and Miquelon (french Republic) */
	{310, 3},	/* United States of America */
	{311, 3},	/* United States of America */
	{312, 3},	/* United States of America */
	{313, 3},	/* United States of America */
	{314, 3},	/* United States of America */
	{315, 3},	/* United States of America */
	{316, 3},	/* United States of America */
	{330, 3},	/* Puerto Rico */
	{332, 3},	/* United States Virgin Islands */
	{334, 3},	/* Mexico */
	{338, 3},	/* Jamaica */
	{340, 2},	/* Guadeloupe and Martinique (French Departments) */
	{342, 3},	/* Barbados */
	{344, 3},	/* Antigua and Barbuda */
	{346, 3},	/* Cayman Islands */
	{348, 3},	/* British Virgin Islands */
	{350, 3},	/* Bermuda */
	{352, 3},	/* Grenada */
	{354, 3},	/* Montserrat */
	{356, 3},	/* Saint Kitts and Nevis */
	{358, 3},	/* Saint Lucia */
	{360, 3},	/* Saint Vincent and the Grenadines */
	{362, 2},	/* Curazao, St Maarten, Bonaire, St Eustatius, Saba */
	{363, 2},	/* Aruba */
	{364, 3},	/* Bahamas (Commonwealth of the) */
	{365, 3},	/* Anguilla */
	{366, 3},	/* Dominica (Commonwealth of) */
	{368, 2},	/* Cuba */
	{370, 2},	/* Dominican Republic */
	{372, 2},	/* Haiti (Republic of) */
	{374, 2},	/* Trinidad and Tobago */
	{376, 3},	/* Turks and Caicos Islands */
	{400, 2},	/* Azerbaijani Republic */
	{401, 2},	/* Kazakhstan (Republic of) */
	{402, 2},	/* Bhutan (Kingdom of) */
	{404, 2},	/* India (Republic of) */
	{405, 2},	/* India (Republic of) */
	{406, 2},	/* India (Republic of) */
	{410, 2},	/* Pakistan (Islamic Republic of) */
	{412, 2},	/* Afghanistan */
	{413, 2},	/* Sri Lanka (Democratic Socialist Republic of) */
	{414, 2},	/* Myanmar (the Republic of the Union of) */
	{415, 2},	/* Lebanon */
	{416, 2},	/* Jordan (Hashemite Kingdom of) */
	{417, 2},	/* Syrian Arab Republic */
	{418, 2},	/* Iraq (Republic of) */
	{419, 2},	/* Kuwait (State of) */
	{420, 2},	/* Saudi Arabia (Kingdom of) */
	{421, 2},	/* Yemen (Republic of) */
	{422, 2},	/* Oman (Sultanate of) */
	{424, 2},	/* United Arab Emirates */
	{425, 2},	/* Israel (State of) */
	{426, 2},	/* Bahrain (Kingdom of) */
	{427, 2},	/* Qatar (State of) */
	{428, 2},	/* Mongolia */
	{429, 2},	/* Nepal (Federal Democratic Republic of) */
	{430, 2},	/* United Arab Emirates */
	{431, 2},	/* United Arab Emirates */
	{432, 2},	/* Iran (Islamic Republic of) */
	{434, 2},	/* Uzbekistan (Republic of) */
	{436, 2},	/* Tajikistan (Republic of) */
	{437, 2},	/* Kyrgyz Republic */
	{438, 2},	/* Turkmenistan */
	{440, 2},	/* Japan */
	{441, 2},	/* Japan */
	{450, 2},	/* Korea (Republic of) */
	{452, 2},	/* Viet Nam (Socialist Republic of) */
	{454, 2},	/* Hong Kong, China */
	{455, 2},	/* Macao, China */
	{456, 2},	/* Cambodia (Kingdom of) */
	{457, 2},	/* Lao People's Democratic Republic */
	{460, 2},	/* China (People's Republic of) */
	{461, 2},	/* China (People's Republic of) */
	{466, 2},	/* Taiwan, China */
	{467, 2},	/* Democratic People's Republic of Korea */
	{470, 2},	/* Bangladesh (People's Republic of) */
	{472, 2},	/* Maldives (Republic of) */
	{502, 2},	/* Malaysia */
	{505, 2},	/* Australia */
	{510, 2},	/* Indonesia (Republic of) */
	{514, 2},	/* Democratic Republic of Timor-Leste */
	{515, 2},	/* Philippines (Republic of the) */
	{520, 2},	/* Thailand */
	{525, 2},	/* Singapore (Republic of) */
	{528, 2},	/* Brunei Darussalam */
	{530, 2},	/* New Zealand */
	{536, 2},	/* Nauru (Republic of) */
	{537, 2},	/* Papua New Guinea */
	{539, 2},	/* Tonga (Kingdom of) */
	{540, 2},	/* Solomon Islands */
	{541, 2},	/* Vanuatu (Republic of) */
	{542, 2},	/* Fiji (Republic of) */
	{543, 2},	/* Wallis and Futuna (french territory) */
	{544, 2},	/* American Samoa */
	{545, 2},	/* Kiribati (Republic of) */
	{546, 2},	/* New Caledonia (french territory) */
	{547, 2},	/* French Polynesia (french territory) */
	{548, 2},	/* Cook Islands */
	{549, 2},	/* Samoa (Independent State of) */
	{550, 2},	/* Micronesia (Federated States of) */
	{551, 2},	/* Marshall Islands (Republic of the) */
	{552, 2},	/* Palau (Republic of) */
	{553, 2},	/* Tuvalu */
	{555, 2},	/* Niue */
	{602, 2},	/* Egypt (Arab Republic of) */
	{603, 2},	/* Algeria (People's Democratic Republic of) */
	{604, 2},	/* Morocco (Kingdom of) */
	{605, 2},	/* Tunisia */
	{606, 2},	/* Libya */
	{607, 2},	/* Gambia (Republic of the) */
	{608, 2},	/* Senegal (Republic of) */
	{609, 2},	/* Mauritania (Islamic Republic of) */
	{610, 2},	/* Mali (Republic of) */
	{611, 2},	/* Guinea (Republic of) */
	{612, 2},	/* Ivory Coast (Republic of) */
	{613, 2},	/* Burkina Faso */
	{614, 2},	/* Niger (Republic of the) */
	{615, 2},	/* Togolese Republic */
	{616, 2},	/* Benin (Republic of) */
	{617, 2},	/* Mauritius (Republic of) */
	{618, 2},	/* Liberia (Republic of) */
	{619, 2},	/* Sierra Leone */
	{620, 2},	/* Ghana */
	{621, 2},	/* Nigeria (Federal Republic of) */
	{622, 2},	/* Chad (Republic of) */
	{623, 2},	/* Central African Republic */
	{624, 2},	/* Cameroon (Republic of) */
	{625, 2},	/* Cape Verde (Republic of) */
	{626, 2},	/* Sao Tome and Principe (Democratic Republic of) */
	{627, 2},	/* Equatorial Guinea (Republic of) */
	{628, 2},	/* Gabonese Republic */
	{629, 2},	/* Congo (Republic of the) */
	{630, 2},	/* Democratic Republic of the Congo */
	{631, 2},	/* Angola (Republic of) */
	{632, 2},	/* Guinea-Bissau (Republic of) */
	{633, 2},	/* Seychelles (Republic of) */
	{634, 2},	/* Sudan (Republic of the) */
	{635, 2},	/* Rwanda (Republic of) */
	{636, 2},	/* Ethiopia (Federal Democratic Republic of) */
	{637, 2},	/* Somali Democratic Republic */
	{638, 2},	/* Djibouti (Republic of) */
	{639, 2},	/* Kenya (Republic of) */
	{640, 2},	/* Tanzania (United Republic of) */
	{641, 2},	/* Uganda (Republic of) */
	{642, 2},	/* Burundi (Republic of) */
	{643, 2},	/* Mozambique (Republic of) */
	{645, 2},	/* Zambia (Republic of) */
	{646, 2},	/* Madagascar (Republic of) */
	{647, 2},	/* French Departments in the Indian Ocean */
	{648, 2},	/* Zimbabwe (Republic of) */
	{649, 2},	/* Namibia (Republic of) */
	{650, 2},	/* Malawi */
	{651, 2},	/* Lesotho (Kingdom of) */
	{652, 2},	/* Botswana (Republic of) */
	{653, 2},	/* Swaziland (Kingdom of) */
	{654, 2},	/* Comoros (Union of the) */
	{655, 2},	/* South Africa (Republic of) */
	{657, 2},	/* Eritrea */
	{658, 2},	/* Saint Helena, Ascension and Tristan da Cunha */
	{659, 2},	/* South Sudan (Republic of) */
	{702, 2},	/* Belize */
	{704, 2},	/* Guatemala (Republic of) */
	{706, 2},	/* El Salvador (Republic of) */
	{708, 3},	/* Honduras (Republic of) */
	{710, 2},	/* Nicaragua */
	{712, 2},	/* Costa Rica */
	{714, 2},	/* Panama (Republic of) */
	{716, 2},	/* Peru */
	{722, 3},	/* Argentine Republic */
	{724, 2},	/* Brazil (Federative Republic of) */
	{730, 2},	/* Chile */
	{732, 3},	/* Colombia (Republic of) */
	{734, 2},	/* Venezuela (Bolivarian Republic of) */
	{736, 2},	/* Bolivia (Plurinational State of) */
	{738, 2},	/* Guyana */
	{740, 2},	/* Ecuador */
	{742, 2},	/* French Guiana (French Department of) */
	{744, 2},	/* Paraguay (Republic of) */
	{746, 2},	/* Suriname (Republic of) */
	{748, 2},	/* Uruguay (Eastern Republic of) */
	{750, 3},	/* Falkland Islands (Malvinas) */
	{901, 2},	/* International Mobile, shared code */
};

/*
 * These MCC+MNC combinations have 3 digit MNC even though the default for
 * the corresponing MCC in mnclen_db is length 2.
 */
static int codes_mnclen3_db[] = {
	374130, 374140, 405000, 405005, 405006,	405007, 405009, 405010,
	405011, 405012, 405013, 405014, 405018,	405020, 405021, 405022,
	405025, 405027, 405029,	405030, 405031, 405032,	405033, 405034,
	405035, 405036, 405037,	405038, 405039, 405040,	405041, 405042,
	405043, 405044, 405045,	405046, 405047,	405750,	405751, 405752,
	405753, 405754, 405755,	405799, 405800,	405801, 405802, 405803,
	405804, 405805,	405806, 405807, 405808,	405809, 714020
};

static int comp_int(const void *key, const void *value)
{
	int mccmnckey = *(int *) key;
	int mccmnccurr = *(int *) value;

	return mccmnckey - mccmnccurr;
}

static int comp_mcc(const void *key, const void *value)
{
	int mcc = *(int *) key;
	struct mcc_mnclength *mccmnc = (struct mcc_mnclength *) value;

	return mcc - mccmnc->mcc;
}

static int mnclength_get_mnclength(const char *imsi)
{
	char mccmnc[OFONO_MAX_MCC_LENGTH + OFONO_MAX_MNC_LENGTH + 1];
	int mccmnc_num;
	int *mccmnc3_res;
	int mcc_num;
	struct mcc_mnclength *mccmnc_res;
	char *endp;

	if (imsi == NULL || *imsi == '\0')
		return -EINVAL;

	/* Special case for some operators */
	strncpy(mccmnc, imsi, sizeof(mccmnc) - 1);
	mccmnc[sizeof(mccmnc) - 1] = '\0';

	mccmnc_num = (int) strtoul(mccmnc, &endp, 10);
	if (*endp != '\0')
		return -EINVAL;

	mccmnc3_res = bsearch(&mccmnc_num, codes_mnclen3_db,
				G_N_ELEMENTS(codes_mnclen3_db),
				sizeof(codes_mnclen3_db[0]), comp_int);
	if (mccmnc3_res)
		return 3;

	/* General case */
	mccmnc[OFONO_MAX_MCC_LENGTH] = '\0';
	mcc_num = (int) strtoul(mccmnc, &endp, 10);
	if (*endp != '\0')
		return -EINVAL;

	mccmnc_res =
		bsearch(&mcc_num, mnclen_db, G_N_ELEMENTS(mnclen_db),
			sizeof(mnclen_db[0]), comp_mcc);
	if (mccmnc_res)
		return mccmnc_res->mnclength;

	return -ENOENT;
}

static struct ofono_sim_mnclength_driver mnclength_driver = {
	.name		= "MNC length",
	.get_mnclength	= mnclength_get_mnclength
};

static int mnclength_init(void)
{
	return ofono_sim_mnclength_driver_register(&mnclength_driver);
}

static void mnclength_exit(void)
{
	ofono_sim_mnclength_driver_unregister(&mnclength_driver);
}

OFONO_PLUGIN_DEFINE(mnclength, "MNC length Plugin", VERSION,
			OFONO_PLUGIN_PRIORITY_DEFAULT,
			mnclength_init, mnclength_exit)
