// ===========================================================================
// BerryBasiC — expression evaluator: functions, primaries, precedence ladder
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// Byte-equality of two n-byte spans (used by the string search/replace helpers).
static int mem_eq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

// Parse an array reference written with empty parentheses, NAME() , as used by
// SPLIT and JOIN$. Fills *name and leaves the caller to find/create the array.
static int parse_array_ref(char *name) {
    if (tok != T_VAR) { err("Expected an array name like parts$()"); return 0; }
    s_copy(name, tok_var, NAME_LEN);
    lex_next();
    if (!expect(T_LP)) return 0;
    return expect(T_RP);
}

static int gpio_guard(void);            // defined with the GPIO statements below

// Render a number as text under a FORMAT$/PRINT USING template. Returns the
// output length, or -1 if the template has no digit position at all. Template
// chars: '#' = optional digit (blank-filled), '0' = zero-filled digit, '.' =
// decimal point (positions after it set the decimal places, value is rounded),
// ',' = thousands separators in the integer part, a leading '+' forces a sign,
// a leading '-' is the default (sign only when negative). Any other character
// is literal and copied through. The integer part is never truncated: if it
// needs more digits than the template provides, the field simply grows.
static int fmt_number(char *out, const char *tmpl, int tlen, double v) {
    int sign_mode = 0;                       // 0 = sign only if negative, 1 = always
    char prefix[64]; int plen = 0;           // literal text before the digit field
    char suffix[64]; int slen = 0;           // literal text after the digit field
    int int_slots = 0, int_zeros = 0;        // integer digit positions / zero-filled ones
    int use_commas = 0, dec_places = 0;
    int seen_field = 0, in_frac = 0, field_done = 0;

    for (int i = 0; i < tlen; i++) {
        char c = tmpl[i];
        if (!seen_field && (c == '+' || c == '-')) {   // leading sign indicator
            if (c == '+') sign_mode = 1;
            continue;
        }
        if (c == '#' || c == '0') {
            if (field_done) { if (slen < 63) suffix[slen++] = c; continue; }
            seen_field = 1;
            if (in_frac) dec_places++;
            else { int_slots++; if (c == '0') int_zeros++; }
            continue;
        }
        if (c == ',' && seen_field && !in_frac && !field_done) { use_commas = 1; continue; }
        if (c == '.' && seen_field && !in_frac && !field_done) { in_frac = 1; continue; }
        if (!seen_field) { if (plen < 63) prefix[plen++] = c; }        // literal prefix
        else { field_done = 1; if (slen < 63) suffix[slen++] = c; }    // literal suffix
    }
    if (int_slots == 0 && dec_places == 0) return -1;    // no digit position

    double mag = v < 0 ? -v : v;
    char sign = v < 0 ? '-' : (sign_mode ? '+' : 0);
    double pw10 = 1; for (int i = 0; i < dec_places; i++) pw10 *= 10;
    double m = mag * pw10;
    // Round half away from zero, with a tiny relative nudge so decimals that a
    // human writes exactly (e.g. 2.005) don't fall short through binary error.
    unsigned long long scaled = (unsigned long long)(m + 0.5 + m * 1e-12 + 1e-9);
    unsigned long long divp = 1; for (int i = 0; i < dec_places; i++) divp *= 10;
    unsigned long long ip = scaled / divp;
    unsigned long long fp = scaled - ip * divp;

    // Integer digits, least-significant first, then zero-padded to the minimum.
    char idig[40]; int ndig = 0;
    if (ip == 0) idig[ndig++] = '0';
    while (ip > 0 && ndig < 39) { idig[ndig++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
    while (ndig < int_zeros && ndig < 39) idig[ndig++] = '0';

    // Weave commas in (still least-significant first), then reverse to forward.
    char istr[64]; int ilen = 0;
    for (int i = 0; i < ndig && ilen < 62; i++) {
        if (use_commas && i > 0 && i % 3 == 0) istr[ilen++] = ',';
        istr[ilen++] = idig[i];
    }
    char ifwd[64]; for (int i = 0; i < ilen; i++) ifwd[i] = istr[ilen - 1 - i];

    // Assemble: prefix, leading blanks (a floating sign eats one), digits,
    // decimal point and fraction, suffix.
    int n = 0;
    for (int i = 0; i < plen && n < MAX_STR; i++) out[n++] = prefix[i];
    int blanks = int_slots - ndig; if (blanks < 0) blanks = 0;
    if (sign) {
        for (int i = 0; i < blanks - 1 && n < MAX_STR; i++) out[n++] = ' ';
        if (n < MAX_STR) out[n++] = sign;
    } else {
        for (int i = 0; i < blanks && n < MAX_STR; i++) out[n++] = ' ';
    }
    for (int i = 0; i < ilen && n < MAX_STR; i++) out[n++] = ifwd[i];
    if (dec_places > 0) {
        if (n < MAX_STR) out[n++] = '.';
        char fbuf[24]; unsigned long long t = fp;
        for (int i = dec_places - 1; i >= 0; i--) { fbuf[i] = (char)('0' + (int)(t % 10)); t /= 10; }
        for (int i = 0; i < dec_places && n < MAX_STR; i++) out[n++] = fbuf[i];
    }
    for (int i = 0; i < slen && n < MAX_STR; i++) out[n++] = suffix[i];
    return n;
}

static value_t eval_function(int fn) {
    lex_next();                         // consume function name

    // Single-argument math functions: paren-optional, argument is the next factor.
    switch (fn) {
        case KW_ABS: { double x = factor_num(); return v_num(x < 0 ? -x : x); }
        case KW_INT: return v_num(dfloor(factor_num()));
        case KW_SGN: { double x = factor_num(); return v_num((x > 0) - (x < 0)); }
        case KW_SQR: { double x = factor_num();
                       if (x < 0) { err("Invalid argument"); return v_num(0); }
                       return v_num(dsqrt(x)); }
        case KW_SIN: return v_num(dsin(factor_num()));
        case KW_COS: return v_num(dcos(factor_num()));
        case KW_TAN: return v_num(dtan(factor_num()));
        case KW_ATN: return v_num(datan(factor_num()));
        case KW_DEG: return v_num(factor_num() * 180.0 / BAS_PI);
        case KW_RAD: return v_num(factor_num() * BAS_PI / 180.0);
        case KW_EXP: return v_num(dexp(factor_num()));
        case KW_LOG: { double x = factor_num();
                       if (x <= 0) { err("Invalid argument"); return v_num(0); }
                       return v_num(dlog(x)); }
        case KW_ASN: { double x = factor_num();         // arcsin via atan
                       if (x < -1 || x > 1) { err("Invalid argument"); return v_num(0); }
                       return v_num(datan(x / dsqrt(1 - x * x))); }
        case KW_ACS: { double x = factor_num();         // arccos = pi/2 - arcsin
                       if (x < -1 || x > 1) { err("Invalid argument"); return v_num(0); }
                       return v_num(BAS_HALFPI - datan(x / dsqrt(1 - x * x))); }

        // File I/O value functions. OPEN* take a filename string; the BGET/EOF/
        // EXT/PTR family take a '#channel'.
        case KW_OPENIN:  { value_t s = need_str(); if (g_err) return v_num(0);
                           char nm[64]; copy_fname(s, nm, sizeof nm);
                           return v_num((double)stg_open(nm, STG_M_READ)); }
        case KW_OPENOUT: { value_t s = need_str(); if (g_err) return v_num(0);
                           char nm[64]; copy_fname(s, nm, sizeof nm);
                           return v_num((double)stg_open(nm, STG_M_WRITE)); }
        case KW_OPENUP:  { value_t s = need_str(); if (g_err) return v_num(0);
                           char nm[64]; copy_fname(s, nm, sizeof nm);
                           return v_num((double)stg_open(nm, STG_M_UPDATE)); }
        case KW_BGET: { int ch = read_channel(); if (g_err) return v_num(0);
                        return v_num((double)stg_getb(ch)); }   // -1 at end of file
        case KW_EOF:  { int ch = read_channel(); if (g_err) return v_num(0);
                        return v_num(stg_eof(ch) ? -1.0 : 0.0); }
        case KW_EXT:  { int ch = read_channel(); if (g_err) return v_num(0);
                        return v_num((double)stg_size(ch)); }
        case KW_PTR:  { int ch = read_channel(); if (g_err) return v_num(0);
                        return v_num((double)stg_tell(ch)); }
        // DIROPEN "path" begins scanning a directory. Returns TRUE if the scan
        // started, FALSE if the directory could not be opened. Walk it with
        // DIRNEXT, read each entry via DIRNAME$/DIRSIZE/DIRTYPE/DIRDATE$/DIRTIME$.
        case KW_DIROPEN: { value_t s = need_str(); if (g_err) return v_num(0);
                           char nm[128]; copy_fname(s, nm, sizeof nm);
                           g_dir_valid = 0;
                           return v_num(stg_diropen(nm) == 0 ? -1.0 : 0.0); }
    }

    // All other functions keep the parenthesised form: FUNC(arg[,arg...]).
    if (!expect(T_LP)) return v_num(0);
    value_t result = v_num(0);
    switch (fn) {
        case KW_INKEY:  { int n = (int)need_num(); result = v_num((double)bas_inkey(n)); break; }
        case KW_INKEYS: { int n = (int)need_num(); int k = bas_inkey(n);
                          if (k < 0) result = v_str(0, 0);   // timeout -> empty string
                          else { char c = (char)k; result = str_in_scratch(&c, 1); }
                          break; }
        case KW_RND: { double x = need_num();         // BBC: RND(1)->[0,1), RND(n>1)->1..n
                       double res;
                       if (x == 1)      res = rnd_float();
                       else if (x == 0) res = rnd_last;
                       else if (x < 0)  { rnd_seed = (unsigned long)(-x) | 1UL; res = x; }
                       else             res = (double)(1 + (long)(rnd_float() * x));
                       result = v_num(res); break; }
        case KW_LEN: { value_t s = need_str(); result = v_num(s.len); break; }
        case KW_ASC: { value_t s = need_str();
                       if (s.len == 0) { err("Invalid argument"); break; }
                       result = v_num((unsigned char)s.str[0]); break; }
        case KW_VAL: { value_t s = need_str();
                       result = v_num(parse_double(s.str, s.len, 0)); break; }
        case KW_POINT: { int px = (int)need_num(); if (!expect(T_COMMA)) break;
                         int py = (int)need_num(); if (g_err) break;
                         result = v_num((double)con_point(px, py)); break; }

        // --- Collections (see the NEW* creators) --------------------------------
        case KW_SIZE: { double h = need_num(); if (g_err) break;
                        void *o = coll_get(h, 0); if (!o) break;
                        result = v_num(coll_size_of(colls[(int)h - 1].type, o)); break; }
        case KW_DICTGET: case KW_DICTGETS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            value_t k = need_str(); if (g_err) break;
            dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) break;
            int i = dict_find(D, k.str, k.len);
            if (i < 0) result = (fn == KW_DICTGETS) ? v_str(0, 0) : v_num(0);   // absent: ""/0
            else       result = (fn == KW_DICTGETS) ? cval_strv(&D->e[i].val) : cval_num(&D->e[i].val);
            break; }
        case KW_DICTHAS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            value_t k = need_str(); if (g_err) break;
            dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) break;
            result = v_num(dict_find(D, k.str, k.len) >= 0 ? -1.0 : 0.0); break; }
        case KW_DICTKEYS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            int i = (int)need_num(); if (g_err) break;
            dict_t *D = (dict_t *)coll_get(h, CT_DICT); if (!D) break;
            if (i < 0 || i >= D->len) { err("Index out of range"); break; }
            result = str_in_scratch(D->e[i].key, D->e[i].klen); break; }
        case KW_POP: case KW_POPS: {
            double h = need_num(); if (g_err) break;
            list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) break;
            if (L->len == 0) { err("List is empty"); break; }
            cval_t *c = &L->item[L->len - 1];
            if (fn == KW_POPS) result = cval_strv(c); else result = cval_num(c);
            if (g_err) break;                                  // wrong type: leave item in place
            cval_clear(c); L->len--; break; }
        case KW_LISTGET: case KW_LISTGETS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            int i = (int)need_num(); if (g_err) break;
            list_t *L = (list_t *)coll_get(h, CT_LIST); if (!L) break;
            if (i < 0 || i >= L->len) { err("Index out of range"); break; }
            result = (fn == KW_LISTGETS) ? cval_strv(&L->item[i]) : cval_num(&L->item[i]); break; }
        case KW_TREEGET: case KW_TREEGETS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            double key = need_num(); if (g_err) break;
            tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) break;
            tnode_t *n = tree_find(T, key);
            if (!n) result = (fn == KW_TREEGETS) ? v_str(0, 0) : v_num(0);       // absent: ""/0
            else    result = (fn == KW_TREEGETS) ? cval_strv(&n->val) : cval_num(&n->val);
            break; }
        case KW_TREEHAS: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            double key = need_num(); if (g_err) break;
            tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) break;
            result = v_num(tree_find(T, key) ? -1.0 : 0.0); break; }
        case KW_TREEMIN: case KW_TREEMAX: {
            double h = need_num(); if (g_err) break;
            tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) break;
            tnode_t *n = tree_edge(T, fn == KW_TREEMAX);
            if (!n) { err("Tree is empty"); break; }
            result = v_num(n->key); break; }
        case KW_TREEKEY: {
            double h = need_num(); if (!expect(T_COMMA)) break;
            int i = (int)need_num(); if (g_err) break;
            tree_t *T = (tree_t *)coll_get(h, CT_TREE); if (!T) break;
            tnode_t *n = tree_index(T, i);
            if (n) result = v_num(n->key);
            break; }
        case KW_I2CPROBE: { int addr = (int)need_num(); if (g_err) break;
                            if (!i2c_available()) { err("I2C needs real Pi hardware"); break; }
                            result = v_num(i2c_probe(addr) ? -1.0 : 0.0); break; }
        // HEX$/BIN$: the value as a 32-bit integer (BBC two's-complement, so
        // HEX$(-1) = "FFFFFFFF"), optionally zero-padded to a minimum width.
        case KW_HEXS: case KW_BINS: {
            unsigned long val = (unsigned long)(unsigned int)(long)need_num();
            int w = 0;
            if (tok == T_COMMA) { lex_next(); w = (int)need_num(); }
            if (g_err) break;
            int base = (fn == KW_HEXS) ? 16 : 2;
            char tmp[33]; int t = 0;
            if (val == 0) tmp[t++] = '0';
            while (val) { int d = (int)(val % base); tmp[t++] = (char)(d < 10 ? '0' + d : 'A' + d - 10); val /= base; }
            char b[40]; int n = 0;
            for (int i = t; i < w && n < 39; i++) b[n++] = '0';   // pad to at least w digits
            while (t) b[n++] = tmp[--t];                          // reverse into b
            result = str_in_scratch(b, n); break;
        }
        // FORMAT$(template$, value): render a number under a template (decimals,
        // width, zero-pad, thousands, sign); literal template text copies through.
        case KW_FORMATS: {
            value_t tp = need_str(); if (!expect(T_COMMA)) break;
            double val = need_num(); if (g_err) break;
            char b[MAX_STR + 1];
            int n = fmt_number(b, tp.str, tp.len, val);
            if (n < 0) { err("Bad format template"); break; }
            result = str_in_scratch(b, n); break;
        }
        // LOADFONT("file.ttf") -> handle > 0 (also selects it), or 0 on failure.
        case KW_LOADFONT: { value_t s = need_str(); if (g_err) break;
                            char nm[128]; copy_fname(s, nm, sizeof nm);
                            result = v_num((double)ttf_load(nm)); break; }
        // TEXTWIDTH(s$) -> pixel width of s$ in the current font/size/style.
        case KW_TEXTWIDTH: { value_t s = need_str(); if (g_err) break;
                             result = v_num((double)ttf_text_width(s.str, s.len)); break; }
        case KW_RGB: { int r = (int)need_num(); if (!expect(T_COMMA)) break;
                       int g = (int)need_num(); if (!expect(T_COMMA)) break;
                       int b = (int)need_num(); if (g_err) break;
                       // Tagged (bit 30) so GCOL can tell a packed colour from a
                       // logical colour index; low 24 bits are 0xRRGGBB.
                       long v = 0x40000000L | ((long)(r & 255) << 16)
                              | ((long)(g & 255) << 8) | (long)(b & 255);
                       result = v_num((double)v); break; }
        case KW_INSTR: { value_t a = need_str(); if (!expect(T_COMMA)) break;
                         value_t b = need_str(); if (g_err) break;
                         int start = 0;
                         if (tok == T_COMMA) { lex_next(); start = (int)need_num() - 1; if (g_err) break; }
                         if (start < 0) start = 0;
                         int pos = 0;                       // 1-based, 0 = not found
                         for (int i = start; i + b.len <= a.len; i++) {
                             int j = 0;
                             while (j < b.len && a.str[i + j] == b.str[j]) j++;
                             if (j == b.len) { pos = i + 1; break; }
                         }
                         result = v_num(pos); break; }
        case KW_CHRS: { int x = (int)need_num();
                        char c = (char)(x & 0xFF);
                        result = str_in_scratch(&c, 1); break; }
        case KW_STRS: { double x = need_num();
                        char buf[40]; int n = dbl_to_str(buf, x);
                        result = str_in_scratch(buf, n); break; }
        case KW_LEFTS: { value_t s = need_str(); if (!expect(T_COMMA)) break;
                         int n = (int)need_num(); if (g_err) break;
                         if (n < 0) n = 0;
                         if (n > s.len) n = s.len;
                         result = str_in_scratch(s.str, n); break; }
        case KW_RIGHTS: { value_t s = need_str(); if (!expect(T_COMMA)) break;
                          int n = (int)need_num(); if (g_err) break;
                          if (n < 0) n = 0;
                          if (n > s.len) n = s.len;
                          result = str_in_scratch(s.str + (s.len - n), n); break; }
        case KW_MIDS: { value_t s = need_str(); if (!expect(T_COMMA)) break;
                        int start = (int)need_num(); if (g_err) break;
                        int cnt = s.len;                       // default: to end
                        if (tok == T_COMMA) { lex_next(); cnt = (int)need_num(); if (g_err) break; }
                        if (start < 1) start = 1;
                        int from = start - 1;                  // 1-based
                        if (from > s.len) from = s.len;
                        int avail = s.len - from;
                        if (cnt < 0) cnt = 0;
                        if (cnt > avail) cnt = avail;
                        result = str_in_scratch(s.str + from, cnt); break; }
        case KW_STRINGS: { int rep = (int)need_num(); if (!expect(T_COMMA)) break;
                           value_t s = need_str(); if (g_err) break;
                           if (rep < 0) rep = 0;
                           if (rep * s.len > MAX_STR) { err("Text string is too long"); break; }
                           char *p = scratch_alloc(rep * s.len);
                           if (!p) break;
                           for (int i = 0; i < rep; i++)
                               for (int j = 0; j < s.len; j++) p[i * s.len + j] = s.str[j];
                           result = v_str(p, rep * s.len); break; }
        // --- modern string library ------------------------------------------
        case KW_UPPERS: case KW_LOWERS: {           // case conversion
                          value_t s = need_str(); if (g_err) break;
                          if (s.len == 0) { result = v_str(0, 0); break; }
                          char *p = scratch_alloc(s.len); if (!p) break;
                          for (int i = 0; i < s.len; i++) {
                              char c = s.str[i];
                              if (fn == KW_UPPERS) { if (c >= 'a' && c <= 'z') c -= 32; }
                              else                 { if (c >= 'A' && c <= 'Z') c += 32; }
                              p[i] = c;
                          }
                          result = v_str(p, s.len); break; }
        case KW_TRIMS: {                            // strip leading/trailing whitespace
                          value_t s = need_str(); if (g_err) break;
                          int a = 0, b = s.len;
                          while (a < b && is_space(s.str[a])) a++;
                          while (b > a && is_space(s.str[b - 1])) b--;
                          result = str_in_scratch(s.str + a, b - a); break; }
        case KW_REPLACES: {                         // REPLACE$(s,old,new): all occurrences
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t o = need_str(); if (!expect(T_COMMA)) break;
                          value_t nw = need_str(); if (g_err) break;
                          if (o.len == 0) { result = str_in_scratch(s.str, s.len); break; }
                          int cnt = 0;
                          for (int i = 0; i + o.len <= s.len; ) {
                              if (mem_eq(s.str + i, o.str, o.len)) { cnt++; i += o.len; } else i++;
                          }
                          long outlen = (long)s.len + (long)cnt * ((long)nw.len - o.len);
                          if (outlen > MAX_STR) { err("Text string is too long"); break; }
                          char *p = outlen > 0 ? scratch_alloc((int)outlen) : 0;
                          if (outlen > 0 && !p) break;
                          int w = 0;
                          for (int i = 0; i < s.len; ) {
                              if (i + o.len <= s.len && mem_eq(s.str + i, o.str, o.len)) {
                                  for (int k = 0; k < nw.len; k++) p[w++] = nw.str[k];
                                  i += o.len;
                              } else p[w++] = s.str[i++];
                          }
                          result = v_str(p, w); break; }
        case KW_CONTAINS: {                         // CONTAINS(s,sub) -> TRUE/FALSE
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t sub = need_str(); if (g_err) break;
                          int found = (sub.len == 0);
                          for (int i = 0; !found && i + sub.len <= s.len; i++)
                              if (mem_eq(s.str + i, sub.str, sub.len)) found = 1;
                          result = v_num(found ? -1.0 : 0.0); break; }
        case KW_STARTSWITH: {                       // STARTSWITH(s,prefix) -> TRUE/FALSE
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t pre = need_str(); if (g_err) break;
                          int r = (pre.len <= s.len) && mem_eq(s.str, pre.str, pre.len);
                          result = v_num(r ? -1.0 : 0.0); break; }
        case KW_ENDSWITH: {                         // ENDSWITH(s,suffix) -> TRUE/FALSE
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t suf = need_str(); if (g_err) break;
                          int r = (suf.len <= s.len) && mem_eq(s.str + (s.len - suf.len), suf.str, suf.len);
                          result = v_num(r ? -1.0 : 0.0); break; }
        case KW_SPLIT: {                            // SPLIT(s,sep,parts$()) -> pieces stored
                          value_t s = need_str(); if (!expect(T_COMMA)) break;
                          value_t sep = need_str(); if (!expect(T_COMMA)) break;
                          if (g_err) break;
                          char sepbuf[MAX_STR]; int seplen = sep.len;
                          for (int i = 0; i < seplen; i++) sepbuf[i] = sep.str[i];
                          char aname[NAME_LEN]; if (!parse_array_ref(aname)) break;
                          if (!name_is_str(aname)) { err("SPLIT needs a string array, e.g. parts$()"); break; }
                          // s stays valid: it lives in scratch, which the GC never moves.
                          int P;                                   // number of pieces
                          if (seplen == 0) P = s.len;              // empty separator: one per char
                          else { int c = 0;
                                 for (int i = 0; i + seplen <= s.len; ) {
                                     if (mem_eq(s.str + i, sepbuf, seplen)) { c++; i += seplen; } else i++; }
                                 P = c + 1; }
                          arr_t *a = arr_find(aname);
                          if (!a) { int counts[1]; counts[0] = P > 0 ? P : 1;
                                    a = arr_create(aname, 1, counts, 1); if (!a) break; }
                          else if (!a->is_str || a->ndim != 1) {
                                    err("SPLIT needs a one-dimensional string array"); break; }
                          int off = a->off, cap = a->total, stored = 0;
                          if (seplen == 0) {
                              for (int i = 0; i < s.len && stored < cap; i++) {
                                  str_store_to(&arr_strs[off + stored], s.str + i, 1);
                                  if (g_err) break;
                                  stored++;
                              }
                          } else {
                              int start = 0;
                              for (int i = 0; i + seplen <= s.len; ) {
                                  if (mem_eq(s.str + i, sepbuf, seplen)) {
                                      if (stored < cap) {
                                          str_store_to(&arr_strs[off + stored], s.str + start, i - start);
                                          if (g_err) break;
                                          stored++;
                                      }
                                      i += seplen; start = i;
                                  } else i++;
                              }
                              if (!g_err && stored < cap) {
                                  str_store_to(&arr_strs[off + stored], s.str + start, s.len - start); stored++; }
                          }
                          if (g_err) break;
                          result = v_num((double)stored); break; }
        case KW_JOINS: {                            // JOIN$(parts$(),sep[,count]) -> joined string
                          char aname[NAME_LEN]; if (!parse_array_ref(aname)) break;
                          if (!name_is_str(aname)) { err("JOIN$ needs a string array, e.g. parts$()"); break; }
                          arr_t *a = arr_find(aname);
                          if (!a || !a->is_str || a->ndim != 1) {
                              err("JOIN$ needs a one-dimensional string array"); break; }
                          if (!expect(T_COMMA)) break;
                          value_t sep = need_str(); if (g_err) break;
                          char sepbuf[MAX_STR]; int seplen = sep.len;
                          for (int i = 0; i < seplen; i++) sepbuf[i] = sep.str[i];
                          int count = a->total;
                          if (tok == T_COMMA) { lex_next(); count = (int)need_num(); if (g_err) break; }
                          if (count < 0) count = 0;
                          if (count > a->total) count = a->total;
                          long outlen = 0;
                          for (int i = 0; i < count; i++) outlen += arr_strs[a->off + i].slen;
                          if (count > 0) outlen += (long)(count - 1) * seplen;
                          if (outlen > MAX_STR) { err("Text string is too long"); break; }
                          char *p = outlen > 0 ? scratch_alloc((int)outlen) : 0;
                          if (outlen > 0 && !p) break;
                          int w = 0;
                          for (int i = 0; i < count; i++) {
                              if (i > 0) for (int k = 0; k < seplen; k++) p[w++] = sepbuf[k];
                              strdesc_t *d = &arr_strs[a->off + i];
                              for (int k = 0; k < d->slen; k++) p[w++] = d->sptr[k];
                          }
                          result = v_str(p, w); break; }
        // Bitwise shift / rotate on 32-bit integers. SHL/SHR shift in zeros
        // (SHR is logical/unsigned); ASR is an arithmetic right shift that keeps
        // the sign bit; ROL/ROR rotate within 32 bits.
        case KW_SHL: { unsigned x = (unsigned)(int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num(); if (g_err) break;
                       if (n < 0) { err("Invalid argument"); break; }
                       result = v_num((double)(int)(n >= 32 ? 0u : x << n)); break; }
        case KW_SHR: { unsigned x = (unsigned)(int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num(); if (g_err) break;
                       if (n < 0) { err("Invalid argument"); break; }
                       result = v_num((double)(int)(n >= 32 ? 0u : x >> n)); break; }
        case KW_ASR: { int x = (int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num(); if (g_err) break;
                       if (n < 0) { err("Invalid argument"); break; }
                       if (n > 31) n = 31;
                       result = v_num((double)(x >> n)); break; }
        case KW_ROL: { unsigned x = (unsigned)(int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num() & 31; if (g_err) break;
                       result = v_num((double)(int)((x << n) | (x >> ((32 - n) & 31)))); break; }
        case KW_ROR: { unsigned x = (unsigned)(int)need_num(); if (!expect(T_COMMA)) break;
                       int n = (int)need_num() & 31; if (g_err) break;
                       result = v_num((double)(int)((x >> n) | (x << ((32 - n) & 31)))); break; }
        // CALL(handle[,arg...]) -> seed's numeric result.
        // CALL$(handle[,arg...]) -> the string the seed set via set_return_str.
        case KW_CALL:  result = v_num(seed_run_collect()); break;
        case KW_CALLS: { double r = seed_run_collect(); (void)r; if (g_err) break;
                         result = (g_seed_retstr_len >= 0)
                                  ? str_in_scratch(g_seed_retstr, g_seed_retstr_len)
                                  : v_str(0, 0); break; }
        // LOADSPRITE("file") decodes an image into a sprite and returns its
        // address (0 on failure). SPRW/SPRH read the width/height from a sprite's
        // header, so a program can position or scale it.
        case KW_LOADSPRITE: { value_t s = need_str(); if (g_err) break;
                              char nm[64]; copy_fname(s, nm, sizeof nm);
                              result = v_num((double)img_load_sprite(nm)); break; }
        // PEEK(addr) -> the byte at addr (0..255). Familiar alias for ?addr; use
        // ! for a 32-bit word and $ for a string. See also POKE.
        case KW_PEEK: { long a = (long)need_num(); if (g_err) break;
                        result = v_num((double)mem_peekb(a)); break; }
        // PIN(n) -> the level (0/1) of BCM header pin n. The statement form
        // PIN n,v (drive an output) is in exec_statement.
        case KW_PIN: { if (gpio_guard()) break;
                       int p = (int)need_num(); if (g_err) break;
                       if (p < 0 || p > 27) { err("No such pin"); break; }
                       result = v_num((double)gpio_read(p)); break; }
        // PINWAIT(pin, edge, timeout_cs) -> pin when the edge arrives, -1 on
        // timeout. edge: 0 = falling, 1 = rising. timeout in centiseconds.
        case KW_PINWAIT: { if (gpio_guard()) break;
                       int p = (int)need_num(); if (!expect(T_COMMA)) break;
                       int e = (int)need_num(); if (!expect(T_COMMA)) break;
                       int t = (int)need_num(); if (g_err) break;
                       if (p < 0 || p > 27) { err("No such pin"); break; }
                       result = v_num((double)gpio_wait_edge(p, e, t)); break; }
        // EVAL("expr") parses the string as an expression and returns its value
        // (number or string). The interpreter already re-lexes source on the fly,
        // so this just points the lexer at a private copy of the string, runs the
        // ordinary expression evaluator, and restores the lexer. The copy is on
        // the C stack because evaluating the inner expression allocates scratch,
        // which could otherwise overwrite the source we are reading.
        case KW_EVAL: { value_t s = need_str(); if (g_err) break;
                       char buf[MAX_STR + 1];
                       int n = s.len; if (n > MAX_STR) n = MAX_STR;
                       for (int i = 0; i < n; i++) buf[i] = s.str[i];
                       buf[n] = 0;
                       lexstate_t save; lex_save(&save);
                       cur_text = buf; lx = buf; lex_next();
                       result = eval_expr();
                       if (!g_err && tok != T_EOL) err("Syntax error in EVAL string");
                       lex_restore(&save); break; }
        case KW_SPRW: case KW_SPRH: {
                          long a = (long)need_num(); if (g_err) break;
                          long v = 0;
                          if (a) {
                              const unsigned char *p = (const unsigned char *)(uintptr_t)a;
                              if (fn == KW_SPRH) p += 4;
                              v = (long)p[0] | ((long)p[1] << 8) |
                                  ((long)p[2] << 16) | ((long)p[3] << 24);
                          }
                          result = v_num((double)v); break; }
        default: err("Syntax error in expression"); break;
    }
    if (!g_err) expect(T_RP);
    return result;
}

// Read a CR-terminated string from memory (the BBC $ indirection).
static value_t mem_read_str(long a) {
    const unsigned char *p = (const unsigned char *)(uintptr_t)a;
    int n = 0;
    while (n < MAX_STR && p[n] != 0x0D) n++;
    return str_in_scratch((const char *)p, n);
}
static void mem_write_str(long a, const char *s, int len) {
    unsigned char *p = (unsigned char *)(uintptr_t)a;
    if (len > MAX_STR) len = MAX_STR;
    for (int i = 0; i < len; i++) p[i] = (unsigned char)s[i];
    p[len] = 0x0D;                                   // carriage-return terminator
}

static value_t prim_base(void) {
    if (g_err) return v_num(0);
    // Unary indirection: ?addr (byte), !addr (word), $addr (string). The operand
    // is a primary, so ?(A+1) needs parentheses; A?1 is the binary form below.
    if (tok == T_QUERY) { lex_next(); value_t a = prim_base();
        if (g_err) return v_num(0);
        if (a.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        return v_num((double)mem_peekb((long)a.num)); }
    if (tok == T_PLING) { lex_next(); value_t a = prim_base();
        if (g_err) return v_num(0);
        if (a.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        return v_num((double)mem_peekw((long)a.num)); }
    if (tok == T_DOLLAR) { lex_next(); value_t a = prim_base();
        if (g_err) return v_num(0);
        if (a.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        return mem_read_str((long)a.num); }
    if (tok == T_NUM) { value_t v = v_num(tok_num); lex_next(); return v; }
    if (tok == T_STR) {
        int n = 0; while (tok_str[n]) n++;
        value_t v = str_in_scratch(tok_str, n);     // copy: tok_str is reused by the lexer
        lex_next();
        return v;
    }
    if (tok == T_VAR) {
        char name[NAME_LEN];
        s_copy(name, tok_var, NAME_LEN);
        int isstr = name_is_str(name);
        var_t *rec = var_lookup(name);           // before the '(' paths: an array of
        if (rec && !rec->is_rec) rec = 0;        // records is not an ordinary array
        lex_next();
        if (rec) return rec_field_get(rec);      // p.x / e(3).x
        if (tok == T_LP) {                       // function call or array element
            // A `name(args)` where `name` is a defined function is a call; only
            // if no such function exists does it mean an array subscript.
            if (find_fn_def(name) >= 0) {
                value_t v = v_num(0);
                call_named(1, name, &v);
                return v;
            }
            int idx; arr_t *a;
            if (!arr_elem(name, isstr, &idx, &a)) return v_num(0);
            return a->is_str ? v_str(arr_strs[idx].sptr, arr_strs[idx].slen)
                             : v_num(arr_nums[idx]);
        }
        var_t *v = var_find(name);
        return (v && v->is_str) ? v_str(v->s.sptr, v->s.slen)
                                : v_num(v ? v->num : 0);
    }
    if (tok == T_KW) {                       // parenless value keywords
        switch (tok_kw) {
            case KW_PI:    lex_next(); return v_num(BAS_PI);
            case KW_TRUE:  lex_next(); return v_num(-1.0);
            case KW_FALSE: lex_next(); return v_num(0.0);
            case KW_NEWDICT: lex_next(); return v_num(coll_new(CT_DICT, sizeof(dict_t)));
            case KW_NEWLIST: lex_next(); return v_num(coll_new(CT_LIST, sizeof(list_t)));
            case KW_NEWTREE: lex_next(); return v_num(coll_new(CT_TREE, sizeof(tree_t)));
            case KW_POS:   lex_next(); return v_num(con_pos());
            case KW_VPOS:  lex_next(); return v_num(con_vpos());
            case KW_ERR:   lex_next(); return v_num(g_errcode);
            case KW_ERRS:  { lex_next();
                             int n = 0; while (g_errmsg[n]) n++;
                             return str_in_scratch(g_errmsg, n); }
            case KW_PINS:  lex_next();
                           if (!gpio_available()) { err("GPIO needs the Pi, not the host build"); return v_num(0); }
                           return v_num((double)gpio_read_all());
            case KW_SCREENW: lex_next(); return v_num(con_screen_w());
            case KW_SCREENH: lex_next(); return v_num(con_screen_h());
            case KW_FONTHEIGHT: lex_next(); return v_num(ttf_line_height());
            case KW_KEYMOD: lex_next(); return v_num(con_keymods());
            case KW_KEYBOARDS: { lex_next(); const char *c = con_get_keyboard();
                                 int n = 0; while (c[n]) n++;
                                 return str_in_scratch(c, n); }
            case KW_MOUSEX: { int x; lex_next(); con_mouse(&x, 0, 0); return v_num(x); }
            case KW_MOUSEY: { int y; lex_next(); con_mouse(0, &y, 0); return v_num(y); }
            case KW_MOUSEB: { int b; lex_next(); con_mouse(0, 0, &b); return v_num(b); }
            case KW_TIME:  lex_next();
                           return v_num((double)((long long)(con_micros() / 10000ULL) - time_base));
            case KW_FN:  { value_t v = v_num(0); call_proc(1, &v); return v; }
            case KW_GET:   lex_next(); return v_num((double)bas_getkey());
            case KW_GETS:{ lex_next(); char ch = (char)bas_getkey(); return str_in_scratch(&ch, 1); }
            // Directory-scan cursor (see DIROPEN). DIRNEXT advances to the next
            // entry, returning TRUE while there is one; the rest read fields of
            // the entry DIRNEXT last landed on.
            case KW_DIRNEXT: { lex_next();
                               int r = stg_dirnext(&g_dirent);
                               if (r < 0) { err("Disk error"); return v_num(0); }
                               g_dir_valid = (r == 1);
                               return v_num(r == 1 ? -1.0 : 0.0); }
            case KW_DIRNAMES: { lex_next();
                                if (!g_dir_valid) return v_str(0, 0);
                                int n = 0; while (g_dirent.name[n]) n++;
                                return str_in_scratch(g_dirent.name, n); }
            case KW_DIRSIZE: lex_next();
                             return v_num(g_dir_valid ? (double)g_dirent.size : 0.0);
            case KW_DIRTYPE: lex_next();
                             return v_num(g_dir_valid && g_dirent.is_dir ? -1.0 : 0.0);
            case KW_DIRDATES: { lex_next(); char b[12];
                                if (!g_dir_valid) return v_str(0, 0);
                                fmt_date(b, g_dirent.year, g_dirent.month, g_dirent.day);
                                return str_in_scratch(b, 10); }
            case KW_DIRTIMES: { lex_next(); char b[8];
                                if (!g_dir_valid) return v_str(0, 0);
                                fmt_time(b, g_dirent.hour, g_dirent.minute);
                                return str_in_scratch(b, 5); }
        }
    }
    if (tok == T_KW && is_func_kw(tok_kw)) return eval_function(tok_kw);
    if (tok == T_KW && is_seed_kw(tok_kw)) return eval_seed_keyword(tok_kw);
    if (tok == T_LP) {
        lex_next();
        value_t v = eval_expr();
        expect(T_RP);
        return v;
    }
    err("Expected a value or expression");
    return v_num(0);
}

// A primary, plus the binary indirection postfix: base?offset (byte at base+off)
// and base!offset (word at base+off). Binds tighter than the arithmetic ops, so
// P%?I + 1 is (P%?I) + 1, and the offset is itself a primary.
static value_t eval_primary(void) {
    value_t a = prim_base();
    while (!g_err && (tok == T_QUERY || tok == T_PLING)) {
        int op = tok;
        lex_next();
        value_t off = prim_base();
        if (g_err) return a;
        if (a.is_str || off.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        long addr = (long)a.num + (long)off.num;
        a = (op == T_QUERY) ? v_num((double)mem_peekb(addr))
                            : v_num((double)mem_peekw(addr));
    }
    return a;
}

// Exponentiation, tighter than unary minus (so -2^2 = -4), left-associative.
// The exponent is a sign-prefixed primary, so 2^-3 and 2^3^2 (=(2^3)^2) work.
static value_t eval_power(void) {
    value_t a = eval_primary();
    while (!g_err && tok == T_CARET) {
        lex_next();
        int neg = 0;
        while (tok == T_MINUS || tok == T_PLUS) { if (tok == T_MINUS) neg = !neg; lex_next(); }
        value_t b = eval_primary();
        if (g_err) return a;
        if (a.is_str || b.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        a = v_num(dpow(a.num, neg ? -b.num : b.num));
    }
    return a;
}

static value_t eval_unary(void) {
    if (tok == T_MINUS) { lex_next(); value_t v = eval_unary();
                          if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
                          return v_num(-v.num); }
    if (tok == T_PLUS)  { lex_next(); return eval_unary(); }
    if (tok == T_KW && tok_kw == KW_NOT) {        // bitwise complement (NOT TRUE = 0)
        lex_next();
        value_t v = eval_unary();
        if (v.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        return v_num((double)(~(int)v.num));
    }
    return eval_power();
}

static value_t eval_term(void) {
    value_t a = eval_unary();
    while (!g_err && (tok == T_STAR || tok == T_SLASH ||
                      (tok == T_KW && (tok_kw == KW_DIV || tok_kw == KW_MOD)))) {
        int op = tok, kw = tok_kw; lex_next();
        value_t b = eval_unary();
        if (g_err) return a;
        if (a.is_str || b.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        if (op == T_STAR) {
            a = v_num(a.num * b.num);
        } else if (op == T_SLASH) {
            if (b.num == 0) { err("Division by zero"); return v_num(0); }
            a = v_num(a.num / b.num);
        } else {                                  // DIV / MOD: integer, truncate to zero
            long bi = (long)b.num;
            if (bi == 0) { err("Division by zero"); return v_num(0); }
            long ai = (long)a.num;
            a = v_num((double)(kw == KW_DIV ? ai / bi : ai % bi));
        }
    }
    return a;
}

static value_t eval_add(void) {
    value_t a = eval_term();
    while (!g_err && (tok == T_PLUS || tok == T_MINUS)) {
        int op = tok; lex_next();
        value_t b = eval_term();
        if (g_err) return a;
        if (op == T_PLUS && a.is_str && b.is_str) {        // string concatenation
            int total = a.len + b.len;
            if (total > MAX_STR) { err("Text string is too long"); return v_num(0); }
            char *p = scratch_alloc(total);
            if (!p) return v_num(0);
            for (int i = 0; i < a.len; i++) p[i] = a.str[i];
            for (int i = 0; i < b.len; i++) p[a.len + i] = b.str[i];
            a = v_str(p, total);
        } else if (a.is_str || b.is_str) {
            err("Type mismatch: numbers and text can't be mixed"); return v_num(0);
        } else {
            a = v_num(op == T_PLUS ? a.num + b.num : a.num - b.num);
        }
    }
    return a;
}

static int str_cmp(value_t a, value_t b) {
    int n = a.len < b.len ? a.len : b.len;
    for (int i = 0; i < n; i++) {
        unsigned char ca = a.str[i], cb = b.str[i];
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.len == b.len) return 0;
    return a.len < b.len ? -1 : 1;
}

// Relational comparison. BBC BASIC TRUE is -1 (all bits set), FALSE is 0.
static value_t eval_compare(void) {
    value_t a = eval_add();
    if (tok == T_EQ || tok == T_NE || tok == T_LT ||
        tok == T_GT || tok == T_LE || tok == T_GE) {
        int op = tok; lex_next();
        value_t b = eval_add();
        if (g_err) return a;
        long c;
        if (a.is_str && b.is_str)      c = str_cmp(a, b);
        else if (!a.is_str && !b.is_str) c = (a.num > b.num) - (a.num < b.num);
        else { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        int r = 0;
        switch (op) {
            case T_EQ: r = (c == 0); break;
            case T_NE: r = (c != 0); break;
            case T_LT: r = (c <  0); break;
            case T_GT: r = (c >  0); break;
            case T_LE: r = (c <= 0); break;
            case T_GE: r = (c >= 0); break;
        }
        a = v_num(r ? -1.0 : 0.0);
    }
    return a;
}

static value_t eval_and(void) {                   // AND: lower than relational
    value_t a = eval_compare();
    while (!g_err && tok == T_KW && tok_kw == KW_AND) {
        lex_next();
        value_t b = eval_compare();
        if (g_err) return a;
        if (a.is_str || b.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        a = v_num((double)((int)a.num & (int)b.num));
    }
    return a;
}

static value_t eval_expr(void) {                  // OR / EOR: lowest precedence
    value_t a = eval_and();
    while (!g_err && tok == T_KW && (tok_kw == KW_OR || tok_kw == KW_EOR)) {
        int kw = tok_kw; lex_next();
        value_t b = eval_and();
        if (g_err) return a;
        if (a.is_str || b.is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
        a = v_num((double)(kw == KW_OR ? ((int)a.num | (int)b.num)
                                       : ((int)a.num ^ (int)b.num)));
    }
    return a;
}

