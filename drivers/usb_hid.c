#include "usb_hid.h"

// Shared USB HID boot-keyboard decoding for both backends (DWC2 + xHCI).
//
// Keyboard layouts
// ----------------
// The physical keyboard always sends the same HID *usage codes* (0x04 = the key
// in the "Q" position, etc.); what those keys should type depends on the layout
// printed on the caps. We keep one full US-ANSI base table and express every
// other layout as a short list of per-key overrides against it, because the vast
// majority of keys (letters, digits, space, Enter, the keypad) are identical
// everywhere and only a dozen or so symbol keys move around.
//
// Three levels are decoded: unshifted, Shift, and AltGr (the right-Alt key,
// HID modifier bit 0x40) which types the third legend on Nordic/German boards
// (@ $ { [ ] } \ | ~ and the like). Accent "dead keys" are not composed; the
// key types the accent character itself (e.g. Norwegian ¨ types ¨), which is
// what a programming console needs. Output is 8-bit Latin-1, and the framebuffer
// font has glyphs for the whole upper range (æ ø å ä ö ü ß § £ € …).

// --- US-ANSI base tables (HID usage code -> character) ----------------------
static const unsigned char us_normal[104] = {
    0,   0,   0,   0,   'a', 'b', 'c', 'd',
    'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't',
    'u', 'v', 'w', 'x', 'y', 'z', '1', '2',
    '3', '4', '5', '6', '7', '8', '9', '0',
    '\n', 0,  '\b','\t',' ', '-', '=', '[',
    ']', '\\','#', ';', '\'','`', ',', '.',
    '/', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   KEY_HOME, 0, KEY_DEL, KEY_END, 0, KEY_RIGHT,
    KEY_LEFT, KEY_DOWN, KEY_UP, 0, '/', '*', '-', '+',
    '\n','1', '2', '3', '4', '5', '6', '7',
    '8', '9', '0', '.', '\\',0,   0,   '='
};

static const unsigned char us_shift[104] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D',
    'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@',
    '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 0,  '\b','\t',' ', '_', '+', '{',
    '}', '|', '~', ':', '"', '~', '<', '>',
    '?', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   KEY_HOME, 0, KEY_DEL, KEY_END, 0, KEY_RIGHT,
    KEY_LEFT, KEY_DOWN, KEY_UP, 0, '/', '*', '-', '+',
    '\n','1', '2', '3', '4', '5', '6', '7',
    '8', '9', '0', '.', '|', 0,   0,   '='
};

// One override: for HID usage `kc`, the unshifted / shifted / AltGr characters
// (0 = "no character at this level").
typedef struct { unsigned char kc, norm, shft, alt; } kov_t;

// UK ISO: a handful of symbols differ from US.
static const kov_t ov_uk[] = {
    { 0x1F, '2', '"',  0   },   // 2
    { 0x20, '3', 0xA3, 0   },   // 3 -> shift is the pound sign
    { 0x34, '\'','@',  0   },   // ' / @
    { 0x32, '#', '~',  0   },   // non-US # / ~
    { 0x31, '\\','|',  0   },   // \ / |
    { 0x35, '`', 0xAC, 0xA6},   // ` / not-sign / broken-bar
    { 0x64, '\\','|',  0   },   // ISO key
};

// Norwegian (Bokmål). Æ Ø Å plus the AltGr programming symbols.
static const kov_t ov_no[] = {
    { 0x1F, '2',  '"',  '@'  },   // 2  " @
    { 0x20, '3',  '#',  0xA3 },   // 3  # £
    { 0x21, '4',  0xA4, '$'  },   // 4  ¤ $
    { 0x22, '5',  '%',  0x80 },   // 5  % €
    { 0x23, '6',  '&',  0    },   // 6  &
    { 0x24, '7',  '/',  '{'  },   // 7  / {
    { 0x25, '8',  '(',  '['  },   // 8  ( [
    { 0x26, '9',  ')',  ']'  },   // 9  ) ]
    { 0x27, '0',  '=',  '}'  },   // 0  = }
    { 0x2D, '+',  '?',  '\\' },   // + ? backslash
    { 0x2E, 0xB4, '`',  0    },   // ´ ` (accent key)
    { 0x2F, 0xE5, 0xC5, 0    },   // å Å
    { 0x30, 0xA8, '^',  '~'  },   // ¨ ^ ~
    { 0x33, 0xF8, 0xD8, 0    },   // ø Ø
    { 0x34, 0xE6, 0xC6, 0    },   // æ Æ
    { 0x31, '\'', '*',  0    },   // ' *
    { 0x35, '|',  0xA7, 0    },   // | § (top-left)
    { 0x36, ',',  ';',  0    },   // , ;
    { 0x37, '.',  ':',  0    },   // . :
    { 0x38, '-',  '_',  0    },   // - _
    { 0x64, '<',  '>',  '|'  },   // < > | (ISO key)
};

// Danish: as Norwegian, but Æ and Ø are swapped and the top-left key is ½/§.
static const kov_t ov_dk[] = {
    { 0x1F, '2',  '"',  '@'  }, { 0x20, '3',  '#',  0xA3 },
    { 0x21, '4',  0xA4, '$'  }, { 0x22, '5',  '%',  0x80 },
    { 0x23, '6',  '&',  0    }, { 0x24, '7',  '/',  '{'  },
    { 0x25, '8',  '(',  '['  }, { 0x26, '9',  ')',  ']'  },
    { 0x27, '0',  '=',  '}'  }, { 0x2D, '+',  '?',  '\\' },
    { 0x2E, 0xB4, '`',  '|'  }, { 0x2F, 0xE5, 0xC5, 0    },   // å Å
    { 0x30, 0xA8, '^',  '~'  }, { 0x33, 0xE6, 0xC6, 0    },   // æ Æ
    { 0x34, 0xF8, 0xD8, 0    },                               // ø Ø
    { 0x31, '\'', '*',  0    }, { 0x35, 0xBD, 0xA7, 0    },   // ½ §
    { 0x36, ',',  ';',  0    }, { 0x37, '.',  ':',  0    },
    { 0x38, '-',  '_',  0    }, { 0x64, '<',  '>',  '\\' },
};

// Swedish: as Norwegian, but Ä Ö replace Æ Ø and the top-left key is §/½.
static const kov_t ov_se[] = {
    { 0x1F, '2',  '"',  '@'  }, { 0x20, '3',  '#',  0xA3 },
    { 0x21, '4',  0xA4, '$'  }, { 0x22, '5',  '%',  0x80 },
    { 0x23, '6',  '&',  0    }, { 0x24, '7',  '/',  '{'  },
    { 0x25, '8',  '(',  '['  }, { 0x26, '9',  ')',  ']'  },
    { 0x27, '0',  '=',  '}'  }, { 0x2D, '+',  '?',  '\\' },
    { 0x2E, 0xB4, '`',  0    }, { 0x2F, 0xE5, 0xC5, 0    },   // å Å
    { 0x30, 0xA8, '^',  '~'  }, { 0x33, 0xF6, 0xD6, 0    },   // ö Ö
    { 0x34, 0xE4, 0xC4, 0    },                               // ä Ä
    { 0x31, '\'', '*',  0    }, { 0x35, 0xA7, 0xBD, 0    },   // § ½
    { 0x36, ',',  ';',  0    }, { 0x37, '.',  ':',  0    },
    { 0x38, '-',  '_',  0    }, { 0x64, '<',  '>',  '|'  },
};

// German (QWERTZ, T1). Y/Z swapped; ä ö ü ß; @ on AltGr+Q, € on AltGr+E.
static const kov_t ov_de[] = {
    { 0x1C, 'z', 'Z', 0    },   // Y position -> z
    { 0x1D, 'y', 'Y', 0    },   // Z position -> y
    { 0x1F, '2', '"', 0    },   // 2 "
    { 0x20, '3', 0xA7, 0   },   // 3 §
    { 0x21, '4', '$', 0    },   // 4 $
    { 0x22, '5', '%', 0    },   // 5 %
    { 0x23, '6', '&', 0    },   // 6 &
    { 0x24, '7', '/', '{'  },   // 7 / {
    { 0x25, '8', '(', '['  },   // 8 ( [
    { 0x26, '9', ')', ']'  },   // 9 ) ]
    { 0x27, '0', '=', '}'  },   // 0 = }
    { 0x14, 'q', 'Q', '@'  },   // q Q @
    { 0x08, 'e', 'E', 0x80 },   // e E €
    { 0x2D, 0xDF,'?', '\\' },   // ß ? backslash
    { 0x2E, 0xB4,'`', 0    },   // ´ `
    { 0x2F, 0xFC,0xDC,0    },   // ü Ü
    { 0x30, '+', '*', '~'  },   // + * ~
    { 0x33, 0xF6,0xD6,0    },   // ö Ö
    { 0x34, 0xE4,0xC4,0    },   // ä Ä
    { 0x31, '#', '\'',0    },   // # '
    { 0x35, '^', 0xB0,0    },   // ^ °
    { 0x36, ',', ';', 0    },   // , ;
    { 0x37, '.', ':', 0    },   // . :
    { 0x38, '-', '_', 0    },   // - _
    { 0x64, '<', '>', '|'  },   // < > |
};

#define OV(a) (a), (sizeof (a) / sizeof (a)[0])
static const struct { const char *code; const kov_t *ov; int n; } layouts[] = {
    { "US", 0, 0 },
    { "UK", OV(ov_uk) },
    { "NO", OV(ov_no) },
    { "DK", OV(ov_dk) },
    { "SE", OV(ov_se) },
    { "DE", OV(ov_de) },
};
#define N_LAYOUTS ((int)(sizeof layouts / sizeof layouts[0]))

// Resolved active tables (US until hid_set_layout says otherwise).
static unsigned char cur_normal[104], cur_shift[104], cur_altgr[104];
static char cur_code[4] = "US";
static int  resolved = 0;

static void resolve(int li) {
    for (int i = 0; i < 104; i++) {
        cur_normal[i] = us_normal[i];
        cur_shift[i]  = us_shift[i];
        cur_altgr[i]  = 0;
    }
    const kov_t *ov = layouts[li].ov;
    for (int i = 0; i < layouts[li].n; i++) {
        cur_normal[ov[i].kc] = ov[i].norm;
        cur_shift[ov[i].kc]  = ov[i].shft;
        cur_altgr[ov[i].kc]  = ov[i].alt;
    }
    int j = 0; while (layouts[li].code[j] && j < 3) { cur_code[j] = layouts[li].code[j]; j++; }
    cur_code[j] = 0;
    resolved = 1;
}

static int ieq(char a, char b) {                 // ASCII case-insensitive compare
    if (a >= 'a' && a <= 'z') a -= 32;
    if (b >= 'a' && b <= 'z') b -= 32;
    return a == b;
}

int hid_set_layout(const char *code) {
    if (!code) return 0;
    for (int li = 0; li < N_LAYOUTS; li++) {
        const char *c = layouts[li].code;
        if (ieq(code[0], c[0]) && code[0] && ieq(code[1], c[1]) &&
            (code[2] == 0 || code[2] == ' ')) {
            resolve(li);
            return 1;
        }
    }
    return 0;
}

const char *hid_layout_code(void) { return cur_code; }

char hid_to_ascii(uint8_t kc, uint8_t mod) {
    if (kc == 0 || kc >= 104) return 0;
    if (!resolved) resolve(0);                   // default to US on first use
    if (mod & 0x40) return (char)cur_altgr[kc];  // AltGr (right Alt): third legend
    return (char)((mod & 0x22) ? cur_shift[kc] : cur_normal[kc]);  // 0x22 = either Shift
}

char hid_report_char(const uint8_t report[8], uint8_t prev[8]) {
    char out = 0;
    uint8_t mod = report[0];
    for (int i = 2; i < 8; i++) {
        uint8_t kc = report[i];
        if (kc == 0) continue;
        int held = 0;
        for (int j = 2; j < 8; j++)
            if (prev[j] == kc) { held = 1; break; }
        if (!held) { out = hid_to_ascii(kc, mod); break; }
    }
    for (int k = 0; k < 8; k++) prev[k] = report[k];
    return out;
}

int hid_mouse_decode(const uint8_t *report, int len,
                     int *btn, int *dx, int *dy, int *wheel) {
    if (len < 3) return 0;
    *btn   = report[0] & 0x07;
    *dx    = (int)(int8_t)report[1];       // sign-extend the movement deltas
    *dy    = (int)(int8_t)report[2];
    *wheel = (len >= 4) ? (int)(int8_t)report[3] : 0;
    return 1;
}
