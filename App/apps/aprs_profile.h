#ifndef APPS_APRS_PROFILE_H
#define APPS_APRS_PROFILE_H

/* Mutable APRS TX asset block.  APRS Edit owns the UI; APRS TX only consumes
 * this fixed layout when it builds a frame. */
#define APRS_PROFILE_MAGIC        0x36435041u /* "APC6" */
#define APRS_PROFILE_LEN          117u
#define APRS_COMMENT_MAX          89u
#define APRS_POS_DIGITS           13u
#define APRS_CALL_MAX             6u

#define APRS_PROFILE_COMMENT_LEN  4u
#define APRS_PROFILE_ROUTE        5u  /* path in bits 5:4, SSID in bits 3:0 */
#define APRS_PROFILE_SYMBOL       6u
#define APRS_PROFILE_HEMI         7u
#define APRS_PROFILE_POS          8u
#define APRS_PROFILE_COMMENT      21u
#define APRS_PROFILE_CALL_LEN     110u
#define APRS_PROFILE_CALL         111u

_Static_assert(APRS_PROFILE_POS + APRS_POS_DIGITS == APRS_PROFILE_COMMENT &&
               APRS_PROFILE_COMMENT + APRS_COMMENT_MAX == APRS_PROFILE_CALL_LEN &&
               APRS_PROFILE_CALL + APRS_CALL_MAX == APRS_PROFILE_LEN,
               "APRS profile layout must match its declared length");

#endif
