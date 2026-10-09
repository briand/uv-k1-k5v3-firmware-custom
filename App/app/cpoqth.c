/* Copyright 2026 NR7Y
 * https://github.com/briand
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 */

// Two-letter QTH codes for the copy drill

#include <string.h>

#include "app/cpoqth.h"

// Each table is a run of entries, a two-letter code followed by the name and a
// NUL, ended by the empty entry the closing NUL of the literal makes. A pointer
// table would cost 4 bytes an entry; walking to the pick costs nothing noticeable.
// Names fit two lines of CPO_QTH_NAME_LINE_LEN, wrapped at a space.

// US states, then Canadian provinces and territories: the W/VE contest exchange
static const char STATES_PROVINCES[] =
	"AL" "Alabama\0"        "AK" "Alaska\0"         "AZ" "Arizona\0"
	"AR" "Arkansas\0"       "CA" "California\0"     "CO" "Colorado\0"
	"CT" "Connecticut\0"    "DE" "Delaware\0"       "FL" "Florida\0"
	"GA" "Georgia\0"        "HI" "Hawaii\0"         "ID" "Idaho\0"
	"IL" "Illinois\0"       "IN" "Indiana\0"        "IA" "Iowa\0"
	"KS" "Kansas\0"         "KY" "Kentucky\0"       "LA" "Louisiana\0"
	"ME" "Maine\0"          "MD" "Maryland\0"       "MA" "Massachusetts\0"
	"MI" "Michigan\0"       "MN" "Minnesota\0"      "MS" "Mississippi\0"
	"MO" "Missouri\0"       "MT" "Montana\0"        "NE" "Nebraska\0"
	"NV" "Nevada\0"         "NH" "New Hampshire\0"  "NJ" "New Jersey\0"
	"NM" "New Mexico\0"     "NY" "New York\0"       "NC" "North Carolina\0"
	"ND" "North Dakota\0"   "OH" "Ohio\0"           "OK" "Oklahoma\0"
	"OR" "Oregon\0"         "PA" "Pennsylvania\0"   "RI" "Rhode Island\0"
	"SC" "South Carolina\0" "SD" "South Dakota\0"   "TN" "Tennessee\0"
	"TX" "Texas\0"          "UT" "Utah\0"           "VT" "Vermont\0"
	"VA" "Virginia\0"       "WA" "Washington\0"     "WV" "West Virginia\0"
	"WI" "Wisconsin\0"      "WY" "Wyoming\0"

	"AB" "Alberta\0"        "BC" "British Columbia\0"
	"MB" "Manitoba\0"       "NB" "New Brunswick\0"
	"NL" "Newfoundland and Labrador\0"              "NS" "Nova Scotia\0"
	"NT" "Northwest Territories\0"                  "NU" "Nunavut\0"
	"ON" "Ontario\0"        "PE" "Prince Edward Island\0"
	"QC" "Quebec\0"         "SK" "Saskatchewan\0"   "YT" "Yukon\0";

// Every ISO 3166 code, plus XK for Kosovo, which ISO leaves to user assignment.
// Each is a DXCC entity or covers several (SH is St Helena, Ascension and
// Tristan da Cunha). Entities inside a parent's code, such as the Canaries or
// Corsica, have no two-letter code of their own to drill.
static const char COUNTRIES[] =
	// The 193 UN members plus Palestine, Taiwan and the Vatican
	"AF" "Afghanistan\0"    "AL" "Albania\0"        "DZ" "Algeria\0"
	"AD" "Andorra\0"        "AO" "Angola\0"         "AG" "Antigua and Barbuda\0"
	"AR" "Argentina\0"      "AM" "Armenia\0"        "AU" "Australia\0"
	"AT" "Austria\0"        "AZ" "Azerbaijan\0"     "BS" "Bahamas\0"
	"BH" "Bahrain\0"        "BD" "Bangladesh\0"     "BB" "Barbados\0"
	"BY" "Belarus\0"        "BE" "Belgium\0"        "BZ" "Belize\0"
	"BJ" "Benin\0"          "BT" "Bhutan\0"         "BO" "Bolivia\0"
	"BA" "Bosnia and Herzegovina\0"                 "BW" "Botswana\0"
	"BR" "Brazil\0"         "BN" "Brunei\0"         "BG" "Bulgaria\0"
	"BF" "Burkina Faso\0"   "BI" "Burundi\0"        "CV" "Cape Verde\0"
	"KH" "Cambodia\0"       "CM" "Cameroon\0"       "CA" "Canada\0"
	"CF" "Central African Rep.\0"                   "TD" "Chad\0"
	"CL" "Chile\0"          "CN" "China\0"          "CO" "Colombia\0"
	"KM" "Comoros\0"        "CG" "Congo\0"          "CD" "DR Congo\0"
	"CR" "Costa Rica\0"     "CI" "Ivory Coast\0"    "HR" "Croatia\0"
	"CU" "Cuba\0"           "CY" "Cyprus\0"         "CZ" "Czech Republic\0"
	"DK" "Denmark\0"        "DJ" "Djibouti\0"       "DM" "Dominica\0"
	"DO" "Dominican Republic\0"                     "EC" "Ecuador\0"
	"EG" "Egypt\0"          "SV" "El Salvador\0"    "GQ" "Equatorial Guinea\0"
	"ER" "Eritrea\0"        "EE" "Estonia\0"        "SZ" "Eswatini\0"
	"ET" "Ethiopia\0"       "FJ" "Fiji\0"           "FI" "Finland\0"
	"FR" "France\0"         "GA" "Gabon\0"          "GM" "Gambia\0"
	"GE" "Georgia\0"        "DE" "Germany\0"        "GH" "Ghana\0"
	"GR" "Greece\0"         "GD" "Grenada\0"        "GT" "Guatemala\0"
	"GN" "Guinea\0"         "GW" "Guinea-Bissau\0"  "GY" "Guyana\0"
	"HT" "Haiti\0"          "HN" "Honduras\0"       "HU" "Hungary\0"
	"IS" "Iceland\0"        "IN" "India\0"          "ID" "Indonesia\0"
	"IR" "Iran\0"           "IQ" "Iraq\0"           "IE" "Ireland\0"
	"IL" "Israel\0"         "IT" "Italy\0"          "JM" "Jamaica\0"
	"JP" "Japan\0"          "JO" "Jordan\0"         "KZ" "Kazakhstan\0"
	"KE" "Kenya\0"          "KI" "Kiribati\0"       "KP" "North Korea\0"
	"KR" "South Korea\0"    "KW" "Kuwait\0"         "KG" "Kyrgyzstan\0"
	"LA" "Laos\0"           "LV" "Latvia\0"         "LB" "Lebanon\0"
	"LS" "Lesotho\0"        "LR" "Liberia\0"        "LY" "Libya\0"
	"LI" "Liechtenstein\0"  "LT" "Lithuania\0"      "LU" "Luxembourg\0"
	"MG" "Madagascar\0"     "MW" "Malawi\0"         "MY" "Malaysia\0"
	"MV" "Maldives\0"       "ML" "Mali\0"           "MT" "Malta\0"
	"MH" "Marshall Islands\0"                       "MR" "Mauritania\0"
	"MU" "Mauritius\0"      "MX" "Mexico\0"         "FM" "Micronesia\0"
	"MD" "Moldova\0"        "MC" "Monaco\0"         "MN" "Mongolia\0"
	"ME" "Montenegro\0"     "MA" "Morocco\0"        "MZ" "Mozambique\0"
	"MM" "Myanmar\0"        "NA" "Namibia\0"        "NR" "Nauru\0"
	"NP" "Nepal\0"          "NL" "Netherlands\0"    "NZ" "New Zealand\0"
	"NI" "Nicaragua\0"      "NE" "Niger\0"          "NG" "Nigeria\0"
	"MK" "North Macedonia\0"                        "NO" "Norway\0"
	"OM" "Oman\0"           "PK" "Pakistan\0"       "PW" "Palau\0"
	"PS" "Palestine\0"      "PA" "Panama\0"         "PG" "Papua New Guinea\0"
	"PY" "Paraguay\0"       "PE" "Peru\0"           "PH" "Philippines\0"
	"PL" "Poland\0"         "PT" "Portugal\0"       "QA" "Qatar\0"
	"RO" "Romania\0"        "RU" "Russia\0"         "RW" "Rwanda\0"
	"KN" "Saint Kitts and Nevis\0"                  "LC" "Saint Lucia\0"
	"VC" "St Vincent and the Grenadines\0"          "WS" "Samoa\0"
	"SM" "San Marino\0"     "ST" "Sao Tome and Principe\0"
	"SA" "Saudi Arabia\0"   "SN" "Senegal\0"        "RS" "Serbia\0"
	"SC" "Seychelles\0"     "SL" "Sierra Leone\0"   "SG" "Singapore\0"
	"SK" "Slovakia\0"       "SI" "Slovenia\0"       "SB" "Solomon Islands\0"
	"SO" "Somalia\0"        "ZA" "South Africa\0"   "SS" "South Sudan\0"
	"ES" "Spain\0"          "LK" "Sri Lanka\0"      "SD" "Sudan\0"
	"SR" "Suriname\0"       "SE" "Sweden\0"         "CH" "Switzerland\0"
	"SY" "Syria\0"          "TW" "Taiwan\0"         "TJ" "Tajikistan\0"
	"TZ" "Tanzania\0"       "TH" "Thailand\0"       "TL" "Timor-Leste\0"
	"TG" "Togo\0"           "TO" "Tonga\0"          "TT" "Trinidad and Tobago\0"
	"TN" "Tunisia\0"        "TR" "Turkey\0"         "TM" "Turkmenistan\0"
	"TV" "Tuvalu\0"         "UG" "Uganda\0"         "UA" "Ukraine\0"
	"AE" "United Arab Emirates\0"                   "GB" "United Kingdom\0"
	"US" "United States\0"  "UY" "Uruguay\0"        "UZ" "Uzbekistan\0"
	"VU" "Vanuatu\0"        "VA" "Vatican City\0"   "VE" "Venezuela\0"
	"VN" "Vietnam\0"        "YE" "Yemen\0"          "ZM" "Zambia\0"
	"ZW" "Zimbabwe\0"

	// Territories and other entities
	"AX" "Aland Islands\0"  "AS" "American Samoa\0" "AI" "Anguilla\0"
	"AQ" "Antarctica\0"     "AW" "Aruba\0"          "BM" "Bermuda\0"
	"BQ" "Bonaire, Saba, St Eustatius\0"            "BV" "Bouvet Island\0"
	"VG" "British Virgin Islands\0"                 "KY" "Cayman Islands\0"
	"IO" "Chagos Islands\0" "CX" "Christmas Island\0"
	"CC" "Cocos (Keeling) Is.\0"                    "CK" "Cook Islands\0"
	"CW" "Curacao\0"        "FK" "Falkland Islands\0"
	"FO" "Faroe Islands\0"  "GF" "French Guiana\0"  "PF" "French Polynesia\0"
	"TF" "French Southern Lands\0"                  "GI" "Gibraltar\0"
	"GL" "Greenland\0"      "GP" "Guadeloupe\0"     "GU" "Guam\0"
	"GG" "Guernsey\0"       "HM" "Heard and McDonald Is.\0"
	"HK" "Hong Kong\0"      "IM" "Isle of Man\0"    "JE" "Jersey\0"
	"XK" "Kosovo\0"         "MO" "Macao\0"          "MQ" "Martinique\0"
	"YT" "Mayotte\0"        "MS" "Montserrat\0"     "NC" "New Caledonia\0"
	"NU" "Niue\0"           "NF" "Norfolk Island\0" "MP" "Northern Marianas\0"
	"PN" "Pitcairn Islands\0"                       "PR" "Puerto Rico\0"
	"RE" "Reunion\0"        "BL" "Saint Barthelemy\0"
	"SH" "Saint Helena\0"   "MF" "Saint Martin\0"
	"PM" "Saint Pierre and Miquelon\0"              "SX" "Sint Maarten\0"
	"GS" "S. Georgia and S. Sandwich\0"
	"SJ" "Svalbard and Jan Mayen\0"                 "TK" "Tokelau\0"
	"TC" "Turks and Caicos Islands\0"
	"UM" "US Minor Outlying Is.\0"                  "VI" "US Virgin Islands\0"
	"WF" "Wallis and Futuna\0"                      "EH" "Western Sahara\0";

static const char *NextEntry(const char *entry)
{
	return entry + strlen(entry) + 1;
}

static const char *PickEntry(const char *table, uint32_t r)
{
	uint16_t count = 0;   // COUNTRIES alone is 250

	for (const char *p = table; *p != '\0'; p = NextEntry(p)) {
		count++;
	}
	for (r %= count; r > 0; r--) {
		table = NextEntry(table);
	}
	return table;
}

const char *CPO_Qth_Pick(uint32_t r, char code[3])
{
	// Even odds rather than one pick across both tables, which would make only
	// about one code in five a state or province
	const char *entry = PickEntry((r & 1) ? COUNTRIES : STATES_PROVINCES, r >> 1);

	code[0] = entry[0];
	code[1] = entry[1];
	code[2] = '\0';
	return entry + 2;
}
