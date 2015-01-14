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
	const char *country_code;
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
	{202, 2, "gr"},	/* Greece */
	{204, 2, "nl"},	/* Netherlands (Kingdom of the) */
	{206, 2, "be"},	/* Belgium */
	{208, 2, "fr"},	/* France */
	{212, 2, "mc"},	/* Monaco (Principality of) */
	{213, 2, "ad"},	/* Andorra (Principality of) */
	{214, 2, "es"},	/* Spain */
	{216, 2, "hu"},	/* Hungary */
	{218, 2, "ba"},	/* Bosnia and Herzegovina */
	{219, 2, "hr"},	/* Croatia (Republic of) */
	{220, 2, "rs"},	/* Serbia (Republic of) */
	{222, 2, "it"},	/* Italy */
	{225, 2, "va"},	/* Vatican City State */
	{226, 2, "ro"},	/* Romania */
	{228, 2, "ch"},	/* Switzerland (Confederation of) */
	{230, 2, "cz"},	/* Czech Republic */
	{231, 2, "sk"},	/* Slovak Republic */
	{232, 2, "at"},	/* Austria */
	{234, 2, "gb"},	/* United Kingdom of G. Britain and Northern Ireland */
	{235, 2, "gb"},	/* United Kingdom of G. Britain and Northern Ireland */
	{238, 2, "dk"},	/* Denmark */
	{240, 2, "se"},	/* Sweden */
	{242, 2, "no"},	/* Norway */
	{244, 2, "fi"},	/* Finland */
	{246, 2, "lt"},	/* Lithuania (Republic of) */
	{247, 2, "lv"},	/* Latvia (Republic of) */
	{248, 2, "ee"},	/* Estonia (Republic of) */
	{250, 2, "ru"},	/* Russian Federation */
	{255, 2, "ua"},	/* Ukraine */
	{257, 2, "by"},	/* Belarus (Republic of) */
	{259, 2, "md"},	/* Moldova (Republic of) */
	{260, 2, "pl"},	/* Poland (Republic of) */
	{262, 2, "de"},	/* Germany (Federal Republic of) */
	{266, 2, "gi"},	/* Gibraltar */
	{268, 2, "pt"},	/* Portugal */
	{270, 2, "lu"},	/* Luxembourg */
	{272, 2, "ie"},	/* Ireland */
	{274, 2, "is"},	/* Iceland */
	{276, 2, "al"},	/* Albania (Republic of) */
	{278, 2, "mt"},	/* Malta */
	{280, 2, "cy"},	/* Cyprus (Republic of) */
	{282, 2, "ge"},	/* Georgia */
	{283, 2, "am"},	/* Armenia (Republic of) */
	{284, 2, "bg"},	/* Bulgaria (Republic of) */
	{286, 2, "tr"},	/* Turkey */
	{288, 2, "fo"},	/* Faroe Islands */
	{290, 2, "gl"},	/* Greenland (Denmark) */
	{292, 2, "sm"},	/* San Marino (Republic of) */
	{293, 2, "si"},	/* Slovenia (Republic of) */
	{294, 2, "mk"},	/* The Former Yugoslav Republic of Macedonia */
	{295, 2, "li"},	/* Liechtenstein (Principality of) */
	{297, 2, "me"},	/* Montenegro (Republic of) */
	{302, 3, "ca"},	/* Canada */
	{308, 2, "pm"},	/* Saint Pierre and Miquelon (french Republic) */
	{310, 3, "us"},	/* United States of America */
	{311, 3, "us"},	/* United States of America */
	{312, 3, "us"},	/* United States of America */
	{313, 3, "us"},	/* United States of America */
	{314, 3, "us"},	/* United States of America */
	{315, 3, "us"},	/* United States of America */
	{316, 3, "us"},	/* United States of America */
	{330, 3, "pr"},	/* Puerto Rico */
	{332, 3, "vi"},	/* United States Virgin Islands */
	{334, 3, "mx"},	/* Mexico */
	{338, 3, "jm"},	/* Jamaica */
	{340, 2, "gp"},	/* Guadeloupe and Martinique (French Departments) */
	{342, 3, "bb"},	/* Barbados */
	{344, 3, "ag"},	/* Antigua and Barbuda */
	{346, 3, "ky"},	/* Cayman Islands */
	{348, 3, "vg"},	/* British Virgin Islands */
	{350, 3, "bm"},	/* Bermuda */
	{352, 3, "gd"},	/* Grenada */
	{354, 3, "ms"},	/* Montserrat */
	{356, 3, "kn"},	/* Saint Kitts and Nevis */
	{358, 3, "lc"},	/* Saint Lucia */
	{360, 3, "vc"},	/* Saint Vincent and the Grenadines */
	{362, 2, "ai"},	/* Curazao, St Maarten, Bonaire, St Eustatius, Saba */
	{363, 2, "aw"},	/* Aruba */
	{364, 3, "bs"},	/* Bahamas (Commonwealth of the) */
	{365, 3, "ai"},	/* Anguilla */
	{366, 3, "dm"},	/* Dominica (Commonwealth of) */
	{368, 2, "cu"},	/* Cuba */
	{370, 2, "do"},	/* Dominican Republic */
	{372, 2, "ht"},	/* Haiti (Republic of) */
	{374, 2, "tt"},	/* Trinidad and Tobago */
	{376, 3, "tc"},	/* Turks and Caicos Islands */
	{400, 2, "az"},	/* Azerbaijani Republic */
	{401, 2, "kz"},	/* Kazakhstan (Republic of) */
	{402, 2, "bt"},	/* Bhutan (Kingdom of) */
	{404, 2, "in"},	/* India (Republic of) */
	{405, 2, "in"},	/* India (Republic of) */
	{406, 2, "pk"},	/* India (Republic of) */
	{410, 2, "af"},	/* Pakistan (Islamic Republic of) */
	{412, 2, "lk"},	/* Afghanistan */
	{413, 2, "mm"},	/* Sri Lanka (Democratic Socialist Republic of) */
	{414, 2, "lb"},	/* Myanmar (the Republic of the Union of) */
	{415, 2, "jo"},	/* Lebanon */
	{416, 2, "sy"},	/* Jordan (Hashemite Kingdom of) */
	{417, 2, "iq"},	/* Syrian Arab Republic */
	{418, 2, "kw"},	/* Iraq (Republic of) */
	{419, 2, "sa"},	/* Kuwait (State of) */
	{420, 2, "ye"},	/* Saudi Arabia (Kingdom of) */
	{421, 2, "om"},	/* Yemen (Republic of) */
	{422, 2, "ps"},	/* Oman (Sultanate of) */
	{424, 2, "ae"},	/* United Arab Emirates */
	{425, 2, "il"},	/* Israel (State of) */
	{426, 2, "bh"},	/* Bahrain (Kingdom of) */
	{427, 2, "qa"},	/* Qatar (State of) */
	{428, 2, "mn"},	/* Mongolia */
	{429, 2, "np"},	/* Nepal (Federal Democratic Republic of) */
	{430, 2, "ae"},	/* United Arab Emirates */
	{431, 2, "ae"},	/* United Arab Emirates */
	{432, 2, "ir"},	/* Iran (Islamic Republic of) */
	{434, 2, "uz"},	/* Uzbekistan (Republic of) */
	{436, 2, "tj"},	/* Tajikistan (Republic of) */
	{437, 2, "kg"},	/* Kyrgyz Republic */
	{438, 2, "tm"},	/* Turkmenistan */
	{440, 2, "jp"},	/* Japan */
	{441, 2, "jp"},	/* Japan */
	{450, 2, "kr"},	/* Korea (Republic of) */
	{452, 2, "vn"},	/* Viet Nam (Socialist Republic of) */
	{454, 2, "hk"},	/* Hong Kong, China */
	{455, 2, "mo"},	/* Macao, China */
	{456, 2, "kh"},	/* Cambodia (Kingdom of) */
	{457, 2, "la"},	/* Lao People's Democratic Republic */
	{460, 2, "cn"},	/* China (People's Republic of) */
	{461, 2, "cn"},	/* China (People's Republic of) */
	{466, 2, "tw"},	/* Taiwan, China */
	{467, 2, "kp"},	/* Democratic People's Republic of Korea */
	{470, 2, "bd"},	/* Bangladesh (People's Republic of) */
	{472, 2, "mv"},	/* Maldives (Republic of) */
	{502, 2, "my"},	/* Malaysia */
	{505, 2, "au"},	/* Australia */
	{510, 2, "id"},	/* Indonesia (Republic of) */
	{514, 2, "tl"},	/* Democratic Republic of Timor-Leste */
	{515, 2, "ph"},	/* Philippines (Republic of the) */
	{520, 2, "th"},	/* Thailand */
	{525, 2, "sg"},	/* Singapore (Republic of) */
	{528, 2, "bn"},	/* Brunei Darussalam */
	{530, 2, "nz"},	/* New Zealand */
	{536, 2, "mp"},	/* Nauru (Republic of) */
	{537, 2, "gu"},	/* Papua New Guinea */
	{539, 2, "nr"},	/* Tonga (Kingdom of) */
	{540, 2, "pg"},	/* Solomon Islands */
	{541, 2, "to"},	/* Vanuatu (Republic of) */
	{542, 2, "sb"},	/* Fiji (Republic of) */
	{543, 2, "vu"},	/* Wallis and Futuna (french territory) */
	{544, 2, "fj"},	/* American Samoa */
	{545, 2, "wf"},	/* Kiribati (Republic of) */
	{546, 2, "as"},	/* New Caledonia (french territory) */
	{547, 2, "ki"},	/* French Polynesia (french territory) */
	{548, 2, "nc"},	/* Cook Islands */
	{549, 2, "pf"},	/* Samoa (Independent State of) */
	{550, 2, "ck"},	/* Micronesia (Federated States of) */
	{551, 2, "ws"},	/* Marshall Islands (Republic of the) */
	{552, 2, "fm"},	/* Palau (Republic of) */
	{553, 2, "mh"},	/* Tuvalu */
	{555, 2, "pw"},	/* Niue */
	{602, 2, "eg"},	/* Egypt (Arab Republic of) */
	{603, 2, "dz"},	/* Algeria (People's Democratic Republic of) */
	{604, 2, "ma"},	/* Morocco (Kingdom of) */
	{605, 2, "tn"},	/* Tunisia */
	{606, 2, "ly"},	/* Libya */
	{607, 2, "gm"},	/* Gambia (Republic of the) */
	{608, 2, "sn"},	/* Senegal (Republic of) */
	{609, 2, "mr"},	/* Mauritania (Islamic Republic of) */
	{610, 2, "ml"},	/* Mali (Republic of) */
	{611, 2, "gn"},	/* Guinea (Republic of) */
	{612, 2, "ci"},	/* Ivory Coast (Republic of) */
	{613, 2, "bf"},	/* Burkina Faso */
	{614, 2, "ne"},	/* Niger (Republic of the) */
	{615, 2, "tg"},	/* Togolese Republic */
	{616, 2, "bj"},	/* Benin (Republic of) */
	{617, 2, "mu"},	/* Mauritius (Republic of) */
	{618, 2, "lr"},	/* Liberia (Republic of) */
	{619, 2, "sl"},	/* Sierra Leone */
	{620, 2, "gh"},	/* Ghana */
	{621, 2, "ng"},	/* Nigeria (Federal Republic of) */
	{622, 2, "td"},	/* Chad (Republic of) */
	{623, 2, "cf"},	/* Central African Republic */
	{624, 2, "cm"},	/* Cameroon (Republic of) */
	{625, 2, "cv"},	/* Cape Verde (Republic of) */
	{626, 2, "st"},	/* Sao Tome and Principe (Democratic Republic of) */
	{627, 2, "gq"},	/* Equatorial Guinea (Republic of) */
	{628, 2, "ga"},	/* Gabonese Republic */
	{629, 2, "cg"},	/* Congo (Republic of the) */
	{630, 2, "cg"},	/* Democratic Republic of the Congo */
	{631, 2, "ao"},	/* Angola (Republic of) */
	{632, 2, "gw"},	/* Guinea-Bissau (Republic of) */
	{633, 2, "sc"},	/* Seychelles (Republic of) */
	{634, 2, "sd"},	/* Sudan (Republic of the) */
	{635, 2, "rw"},	/* Rwanda (Republic of) */
	{636, 2, "et"},	/* Ethiopia (Federal Democratic Republic of) */
	{637, 2, "so"},	/* Somali Democratic Republic */
	{638, 2, "dj"},	/* Djibouti (Republic of) */
	{639, 2, "ke"},	/* Kenya (Republic of) */
	{640, 2, "tz"},	/* Tanzania (United Republic of) */
	{641, 2, "ug"},	/* Uganda (Republic of) */
	{642, 2, "bi"},	/* Burundi (Republic of) */
	{643, 2, "mz"},	/* Mozambique (Republic of) */
	{645, 2, "zm"},	/* Zambia (Republic of) */
	{646, 2, "mg"},	/* Madagascar (Republic of) */
	{647, 2, "re"},	/* French Departments in the Indian Ocean */
	{648, 2, "zw"},	/* Zimbabwe (Republic of) */
	{649, 2, "na"},	/* Namibia (Republic of) */
	{650, 2, "mw"},	/* Malawi */
	{651, 2, "ls"},	/* Lesotho (Kingdom of) */
	{652, 2, "bw"},	/* Botswana (Republic of) */
	{653, 2, "sz"},	/* Swaziland (Kingdom of) */
	{654, 2, "km"},	/* Comoros (Union of the) */
	{655, 2, "za"},	/* South Africa (Republic of) */
	{657, 2, "er"},	/* Eritrea */
	{658, 2, "sh"},	/* Saint Helena, Ascension and Tristan da Cunha */
	{659, 2, "ss"},	/* South Sudan (Republic of) */
	{702, 2, "bz"},	/* Belize */
	{704, 2, "gt"},	/* Guatemala (Republic of) */
	{706, 2, "sv"},	/* El Salvador (Republic of) */
	{708, 3, "hn"},	/* Honduras (Republic of) */
	{710, 2, "ni"},	/* Nicaragua */
	{712, 2, "cr"},	/* Costa Rica */
	{714, 2, "pa"},	/* Panama (Republic of) */
	{716, 2, "pe"},	/* Peru */
	{722, 3, "ar"},	/* Argentine Republic */
	{724, 2, "br"},	/* Brazil (Federative Republic of) */
	{730, 2, "cl"},	/* Chile */
	{732, 3, "co"},	/* Colombia (Republic of) */
	{734, 2, "ve"},	/* Venezuela (Bolivarian Republic of) */
	{736, 2, "bo"},	/* Bolivia (Plurinational State of) */
	{738, 2, "gy"},	/* Guyana */
	{740, 2, "ec"},	/* Ecuador */
	{742, 2, "gf"},	/* French Guiana (French Department of) */
	{744, 2, "py"},	/* Paraguay (Republic of) */
	{746, 2, "sr"},	/* Suriname (Republic of) */
	{748, 2, "uy"},	/* Uruguay (Eastern Republic of) */
	{750, 3, "fk"},	/* Falkland Islands (Malvinas) */
	{901, 2, NULL},	/* International Mobile, shared code */
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

static const char *mnclength_get_country_code(const char *mcc)
{
	char buf[OFONO_MAX_MCC_LENGTH + 1];
	char *endp;
	int mcc_num;
	struct mcc_mnclength *mcc_res;

	if (mcc == NULL || *mcc == '\0')
		return NULL;

	strncpy(buf, mcc, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	mcc_num = (int) strtoul(buf, &endp, 10);
	if (*endp != '\0')
		return NULL;

	mcc_res = bsearch(&mcc_num, mnclen_db, G_N_ELEMENTS(mnclen_db),
			sizeof(mnclen_db[0]), comp_mcc);
	if (mcc_res)
		return mcc_res->country_code;

	return NULL;
}

static struct ofono_sim_mnclength_driver mnclength_driver = {
	.name			= "MNC length",
	.get_mnclength		= mnclength_get_mnclength,
	.get_country_code	= mnclength_get_country_code
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
