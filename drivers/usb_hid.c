#include "usb_hid.h"

// HID usage -> ASCII (UK-ish layout, matching the DWC2 driver's original tables).
static const char keys_normal[104] = {
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

static const char keys_shift[104] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D',
    'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T',
    'U', 'V', 'W', 'X', 'Y', 'Z', '!', '"',
    '#', '$', '%', '^', '&', '*', '(', ')',
    '\n', 0,  '\b','\t',' ', '_', '+', '{',
    '}', '|', '~', ':', '@', '~', '<', '>',
    '?', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   KEY_HOME, 0, KEY_DEL, KEY_END, 0, KEY_RIGHT,
    KEY_LEFT, KEY_DOWN, KEY_UP, 0, '/', '*', '-', '+',
    '\n','1', '2', '3', '4', '5', '6', '7',
    '8', '9', '0', '.', '|', 0,   0,   '='
};

char hid_to_ascii(uint8_t kc, uint8_t mod) {
    if (kc == 0 || kc >= 104) return 0;
    return (mod & 0x22) ? keys_shift[kc] : keys_normal[kc];   // 0x22 = either Shift
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
