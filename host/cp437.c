int codepage437_to_unicode(int ch) {
    switch (ch) {
        case 0x01: return 0x263A; // White Smiling Face
        case 0x02: return 0x263B; // Black Smiling Face
        case 0x03: return 0x2665; // Black Heart Suit
        case 0x04: return 0x2666; // Black Diamond Suit
        case 0x05: return 0x2663; // Black Club Suit
        case 0x06: return 0x2660; // Black Spade Suit
        case 0x07: return 0x2022; // Bullet
        case 0x08: return 0x25D8; // Inverse Bullet
        case 0x09: return 0x25CB; // White Circle
        case 0x0A: return 0x25D9; // Inverse White Circle
        case 0x0B: return 0x2642; // Male Sign
        case 0x0C: return 0x2640; // Female Sign
        case 0x0D: return 0x266A; // Eighth Note
        case 0x0E: return 0x266B; // Beamed Eighth Notes
        case 0x0F: return 0x263C; // White Sun With Rays
        case 0x10: return 0x25BA; // Black Right-Pointing Pointer
        case 0x11: return 0x25C4; // Black Left-Pointing Pointer
        case 0x12: return 0x2115; // Up Down Arrow
        case 0x13: return 0x203C; // Double Exclamation Mark
        case 0x14: return 0x00B6; // Pilcrow Sign (Paragraph)
        case 0x15: return 0x00A7; // Section Sign
        case 0x16: return 0x25AC; // Black Rectangle
        case 0x17: return 0x21A8; // Up Down Arrow With Base
        case 0x18: return 0x2191; // Upwards Arrow
        case 0x19: return 0x2193; // Downwards Arrow
        case 0x1A: return 0x2192; // Rightwards Arrow
        case 0x1B: return 0x2190; // Leftwards Arrow
        case 0x1C: return 0x221F; // Right Angle
        case 0x1D: return 0x2194; // Left Right Arrow
        case 0x1E: return 0x25B2; // Black Up-Pointing Triangle
        case 0x1F: return 0x25BC; // Black Down-Pointing Triangle

        case 0x7F: return 0x2302; //   ⌂
        case 0x80: return 0x00C7; //   Ç
        case 0x81: return 0x00FC; //   ü
        case 0x82: return 0x00E9; //   é
        case 0x83: return 0x00E2; //   â
        case 0x84: return 0x00E4; //   ä
        case 0x85: return 0x00E0; //   à
        case 0x86: return 0x00E5; //   å
        case 0x87: return 0x00E7; //   ç
        case 0x88: return 0x00EA; //   ê
        case 0x89: return 0x00EB; //   ë
        case 0x8A: return 0x00E8; //   è
        case 0x8B: return 0x00EF; //   ï
        case 0x8C: return 0x00EE; //   î
        case 0x8D: return 0x00EC; //   ì
        case 0x8E: return 0x00C4; //   Ä
        case 0x8F: return 0x00C5; //   Å
        case 0x90: return 0x00C9; //   É
        case 0x91: return 0x00E6; //   æ
        case 0x92: return 0x00C6; //   Æ
        case 0x93: return 0x00F4; //   ô
        case 0x94: return 0x00F6; //   ö
        case 0x95: return 0x00F2; //   ò
        case 0x96: return 0x00FB; //   û
        case 0x97: return 0x00F9; //   ù
        case 0x98: return 0x00FF; //   ÿ
        case 0x99: return 0x00D6; //   Ö
        case 0x9A: return 0x00DC; //   Ü
        case 0x9B: return 0x00A2; //   ¢
        case 0x9C: return 0x00A3; //   £
        case 0x9D: return 0x00A5; //   ¥
        case 0x9E: return 0x20A7; //   ₧
        case 0x9F: return 0x0192; //   ƒ
        case 0xA0: return 0x00E1; //   á
        case 0xA1: return 0x00ED; //   í
        case 0xA2: return 0x00F3; //   ó
        case 0xA3: return 0x00FA; //   ú
        case 0xA4: return 0x00F1; //   ñ
        case 0xA5: return 0x00D1; //   Ñ
        case 0xA6: return 0x00AA; //   ª
        case 0xA7: return 0x00BA; //   º
        case 0xA8: return 0x00BF; //   ¿
        case 0xA9: return 0x2310; //   ⌐
        case 0xAA: return 0x00AC; //   ¬
        case 0xAB: return 0x00BD; //   ½
        case 0xAC: return 0x00BC; //   ¼
        case 0xAD: return 0x00A1; //   ¡
        case 0xAE: return 0x00AB; //   «
        case 0xAF: return 0x00BB; //   »
        case 0xB0: return 0x2591; //   ░
        case 0xB1: return 0x2592; //   ▒
        case 0xB2: return 0x2593; //   ▓
        case 0xB3: return 0x2502; //   │
        case 0xB4: return 0x2524; //   ┤
        case 0xB5: return 0x2561; //   ╡
        case 0xB6: return 0x2562; //   ╢
        case 0xB7: return 0x2556; //   ╖
        case 0xB8: return 0x2555; //   ╕
        case 0xB9: return 0x2563; //   ╣
        case 0xBA: return 0x2551; //   ║
        case 0xBB: return 0x2557; //   ╗
        case 0xBC: return 0x255D; //   ╝
        case 0xBD: return 0x255C; //   ╜
        case 0xBE: return 0x255B; //   ╛
        case 0xBF: return 0x2510; //   ┐
        case 0xC0: return 0x2514; //   └
        case 0xC1: return 0x2534; //   ┴
        case 0xC2: return 0x252C; //   ┬
        case 0xC3: return 0x251C; //   ├
        case 0xC4: return 0x2500; //   ─
        case 0xC5: return 0x253C; //   ┼
        case 0xC6: return 0x255E; //   ╞
        case 0xC7: return 0x255F; //   ╟
        case 0xC8: return 0x255A; //   ╚
        case 0xC9: return 0x2554; //   ╔
        case 0xCA: return 0x2569; //   ╩
        case 0xCB: return 0x2566; //   ╦
        case 0xCC: return 0x2560; //   ╠
        case 0xCD: return 0x2550; //   ═
        case 0xCE: return 0x256C; //   ╬
        case 0xCF: return 0x2567; //   ╧
        case 0xD0: return 0x2568; //   ╨
        case 0xD1: return 0x2564; //   ╤
        case 0xD2: return 0x2565; //   ╥
        case 0xD3: return 0x2559; //   ╙
        case 0xD4: return 0x2558; //   ╘
        case 0xD5: return 0x2552; //   ╒
        case 0xD6: return 0x2553; //   ╓
        case 0xD7: return 0x256B; //   ╫
        case 0xD8: return 0x256A; //   ╪
        case 0xD9: return 0x2518; //   ┘
        case 0xDA: return 0x250C; //   ┌
        case 0xDB: return 0x2588; //   █
        case 0xDC: return 0x2584; //   ▄
        case 0xDD: return 0x258C; //   ▌
        case 0xDE: return 0x2590; //   ▐
        case 0xDF: return 0x2580; //   ▀
        case 0xE0: return 0x03B1; //   α
        case 0xE1: return 0x00DF; //   ß
        case 0xE2: return 0x0393; //   Γ
        case 0xE3: return 0x03C0; //   π
        case 0xE4: return 0x03A3; //   Σ
        case 0xE5: return 0x03C3; //   σ
        case 0xE6: return 0x00B5; //   µ
        case 0xE7: return 0x03C4; //   τ
        case 0xE8: return 0x03A6; //   Φ
        case 0xE9: return 0x0398; //   Θ
        case 0xEA: return 0x03A9; //   Ω
        case 0xEB: return 0x03B4; //   δ
        case 0xEC: return 0x221E; //   ∞
        case 0xED: return 0x03C6; //   φ
        case 0xEE: return 0x03B5; //   ε
        case 0xEF: return 0x2229; //   ∩
        case 0xF0: return 0x2261; //   ≡
        case 0xF1: return 0x00B1; //   ±
        case 0xF2: return 0x2265; //   ≥
        case 0xF3: return 0x2264; //   ≤
        case 0xF4: return 0x2320; //   ⌠
        case 0xF5: return 0x2321; //   ⌡
        case 0xF6: return 0x00F7; //   ÷
        case 0xF7: return 0x2248; //   ≈
        case 0xF8: return 0x00B0; //   °
        case 0xF9: return 0x2219; //   ∙
        case 0xFA: return 0x00B7; //   ·
        case 0xFB: return 0x221A; //   √
        case 0xFC: return 0x207F; //   ⁿ
        case 0xFD: return 0x00B2; //   ²
        case 0xFE: return 0x25A0; //   ■
        case 0xFF: return 0x00A0; //

        default: return ch;
    }
}