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

// Two-letter QTH codes for the copy drill: US state and Canadian province postal
// codes, and ISO 3166 country codes. Some codes are both (CA, GA, NU), so the
// drill names the one it picked once it is copied.

#ifndef APP_CPOQTH_H
#define APP_CPOQTH_H

#include <stdint.h>

// Names wrap at a space onto a second line of at most this many characters
#define CPO_QTH_NAME_LINE_LEN 14

// Pick a state or province, or a country (even odds), from the random number r:
// copies its code into code and returns its name
const char *CPO_Qth_Pick(uint32_t r, char code[3]);

#endif
