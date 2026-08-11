#ifndef _COMPAT_VALUES_H
#define _COMPAT_VALUES_H
#include <limits.h>
/* Classic <values.h> names. DOOM's doomtype.h also defines some of these; the
 * resulting redefinition warnings are harmless. */
#define MAXINT   INT_MAX
#define MININT   INT_MIN
#define MAXSHORT SHRT_MAX
#define MINSHORT SHRT_MIN
#define MAXLONG  LONG_MAX
#define MINLONG  LONG_MIN
#define MAXCHAR  CHAR_MAX
#define MINCHAR  CHAR_MIN
#endif
