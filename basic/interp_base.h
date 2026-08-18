#ifndef INTERP_BASE_H
#define INTERP_BASE_H

/* Common base for every interpreter module: the platform/library headers
 * and the shared compile-time constants.  Each interp_*.h includes this. */

#include <stdint.h>
#include "console.h"
#include "storage.h"
#include "seed.h"
#include "pod.h"
#include "image.h"
#include "sound.h"
#include "gpio.h"
#include "i2c.h"
#include "ttf.h"
#include "gfx.h"
#include "basic.h"
#include "conf.h"

/* ---- shared constants ---- */
#define LINE_LEN     128     // maximum length of a line
#define MAX_LINES   8192     // total length of a BASIC program
#define MAX_VARS     512     // total number of allowed variables in a program
#define NAME_LEN       8     // variable name incl. trailing '$' for strings
#define MAX_STR      255     // classic BASIC maximum string length
#define GCHEAP_SIZE 16384    // bytes of string storage for variables (GC'd)
#define SCRATCH_SIZE 4096    // per-statement scratch for string temporaries
#define BAS_PI     3.14159265358979323846
#define BAS_HALFPI 1.57079632679489661923
#define BAS_TWOPI  6.28318530717958647692
#define BAS_LN2    0.69314718055994530942
#define ERRMSG_MAX 128
#define MAX_ARRAYS      16
#define MAX_DIMS         3
#define ARR_NUM_POOL  4096      // numeric elements (longs)
#define ARR_STR_POOL   512      // string elements (descriptors)
#define MAX_TYPES       8
#define MAX_FIELDS     16
#define REC_NUM_POOL 1024      // numeric field slots across all record instances
#define REC_STR_POOL  256      // string field slots (descriptors into gcheap)
#define LOCAL_MAX 96
#define DIM_HEAP_SIZE (256 * 1024)   // room for indirection buffers and sprites (GGET/GPUT)
#define KW_SEED_DYN 5000
#define SEED_HEAP_SIZE  (48u * 1024u * 1024u)  // shared by seeds and POD programs;
#define COLL_MAX 64
#define SEED_KW_MAX 256
#define GOSUB_MAX 32
#define FOR_MAX 16
#define REPEAT_MAX 16
#define WHILE_MAX 16
#define TRY_MAX 16
#define FN_RET_MAX 32
#define DEF_MAX 64
#define PRINT_FIELD 8     // column width for the ',' separator and TAB alignment
#define REC_NUM  0x40
#define REC_STR  0x00
#define LC_LINE  6      // line-number gutter  (cyan)
#define LC_KW    3      // keywords            (yellow)
#define LC_STR   2      // string literals     (green)
#define LC_NUM   5      // numeric literals    (magenta)
#define LC_NAME  6      // PROC/FN names       (cyan)
#define LC_REM   4      // REM comments        (blue)
#define LC_DEF   7      // everything else     (white)
#define EV_PIN_MAX 8
#define STG_BUF_SIZE 65536
#define CHAIN_MAX 4
#define MAX_MODULES 16
#define SND_CHANS 4          // BBC channels 0..3
#define SND_QLEN  9          // per-channel ring buffer (8 usable notes)
#define RGB_TAG 0x40000000
#define POD_MAX        4                 // resident/running PODs at once
#define POD_SLOT_SIZE  (2 * 1024 * 1024) // biggest image we host (tcc's is ~0.7 MB)
#define MAX_ARGS 16

#endif /* INTERP_BASE_H */
