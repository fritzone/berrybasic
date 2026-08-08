// ===========================================================================
// BerryBasiC — native seeds: heap, collections (DICT/LIST/TREE), seed-service vtable
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// ---------------------------------------------------------------------------
// Native seeds: small chunks of position-independent AArch64 machine code that
// a program loads (SEED) and calls (CALL / CALL$). The blob is copied into a
// page-aligned, executable RAM slot; seeds reach the interpreter only through
// the BerryServices vtable below, which is what keeps them self-contained. See
// seed/seed.h for the ABI.
// ---------------------------------------------------------------------------
enum { SEED_MAX_ARGS = 16 };

// A loaded SEED/CALL seed. There is no fixed number of them and no fixed size:
// each seed's code is a page-aligned block carved from the run heap and sized to
// the blob, and the record table grows on demand. Both are wiped on RUN/NEW (a
// program reloads its own seeds), so seed_recs is reset to 0 there. Handle =
// record index + 1.
typedef struct { int used; seed_entry entry; } seed_rec_t;
static seed_rec_t *seed_recs;          // grown from the run heap; 0 after a reset
static int         seed_rec_n;         // records touched (high-water within the array)
static int         seed_rec_cap;       // allocated capacity

static void *seed_alloc(unsigned nbytes);   // the run-heap allocator, defined below

// Drop every loaded seed. The blobs and this table live in the run heap, which
// seed_heap_reset() wipes on RUN/NEW, so we just forget the pointers.
static void seed_recs_reset(void) { seed_recs = 0; seed_rec_n = 0; seed_rec_cap = 0; }

// Reserve a fresh record and return its index, or -1 on OOM. Reuses a freed slot
// when one exists, else appends (growing the array from the run heap).
static int seed_rec_alloc(void) {
    for (int i = 0; i < seed_rec_n; i++) if (!seed_recs[i].used) return i;
    if (seed_rec_n >= seed_rec_cap) {
        int ncap = seed_rec_cap ? seed_rec_cap * 2 : 8;
        seed_rec_t *nt = (seed_rec_t *)seed_alloc((unsigned)(ncap * sizeof(seed_rec_t)));
        if (!nt) return -1;
        for (int i = 0; i < seed_rec_n; i++) nt[i] = seed_recs[i];   // old copy leaks until RUN
        seed_recs = nt; seed_rec_cap = ncap;
    }
    return seed_rec_n++;
}

// A page-aligned executable block from the run heap, sized to nbytes (rounded up
// to a page). Not individually freed: reclaimed wholesale by the RUN/NEW reset.
static void *seed_alloc_page(unsigned nbytes) {
    unsigned asz = (nbytes + 4095u) & ~4095u;
    char *raw = (char *)seed_alloc(asz + 4096u);        // slack to reach a page boundary
    if (!raw) return 0;
    return (void *)(((unsigned long)raw + 4095u) & ~(unsigned long)4095u);
}

static char g_seed_retstr[MAX_STR];   // string result staged by set_return_str
static int  g_seed_retstr_len;        // -1 = the last call set no string

// --- seed heap -------------------------------------------------------------
// A general-purpose allocator for seeds (BASIC's own string/array storage is
// separate). Classic K&R first-fit over a fixed arena with coalescing on free;
// returns 0 when exhausted. The whole arena is reclaimed at each RUN/NEW, so a
// seed that forgets to free leaks only within the current run. 16-byte aligned
// blocks, suitable for doubles and NEON.
#define SEED_HEAP_SIZE  (48u * 1024u * 1024u)  // shared by seeds and POD programs;
                                                // tcc-as-a-POD needs elbow room to compile

typedef union seed_hdr_u {
    struct { union seed_hdr_u *next; unsigned size; } s;  // size in header units
    long double _align;                                   // force 16-byte units
} seed_blk;

// 4096-aligned so a page-aligned sub-allocation is a whole number of blocks from
// the base (native seed code needs a page-aligned load address for ADRP+ADD).
static seed_blk seed_heap[SEED_HEAP_SIZE / sizeof(seed_blk)] __attribute__((aligned(4096)));
static seed_blk seed_freelist;        // circular free-list sentinel
static seed_blk *seed_freep;          // 0 until first use / after a reset

// Session-persistent high-water mark. Native seed code (both SEED/CALL blobs and
// keyword seeds) has to run from executable RAM; there is no OS page allocator,
// so it is carved from this same 48 MB arena. Keyword seeds, however, must SURVIVE
// RUN/NEW (they are language extensions loaded once at startup), and the run heap
// below is wiped on every RUN. So keyword-seed code and the keyword table grow
// DOWN from the top of the arena, past seed_sys_top, which the reset leaves alone;
// the K&R free-list only ever covers [seed_heap, seed_sys_top). When no keyword
// seeds are loaded, seed_sys_top is the arena end and the heap behaves as before.
static seed_blk *seed_sys_top;        // 0 until first use; else the persistent floor

static void seed_heap_reset(void) { seed_freep = 0; }     // lazily re-inited

static void seed_heap_init(void) {
    if (!seed_sys_top) seed_sys_top = seed_heap + sizeof(seed_heap) / sizeof(seed_blk);
    seed_blk *base = seed_heap;
    base->s.size = (unsigned)(seed_sys_top - seed_heap);   // run heap = below the floor
    base->s.next = &seed_freelist;
    seed_freelist.s.next = base;
    seed_freelist.s.size = 0;
    seed_freep = &seed_freelist;
}

// Reserve `nbytes` of page-aligned persistent memory from the top of the arena
// for a native keyword seed's code. Keyword seeds are scanned once at startup,
// BEFORE the run heap is first used, so moving the floor down here never disturbs
// a live allocation; if the run heap is already in use we refuse rather than
// corrupt it (return 0). Never individually freed: it lives for the session.
static void *seed_persist_page(unsigned nbytes) {
    if (nbytes == 0) return 0;
    if (seed_freep) return 0;    // run heap already live: only safe before first use (boot)
    if (!seed_sys_top) seed_sys_top = seed_heap + sizeof(seed_heap) / sizeof(seed_blk);
    unsigned units = (nbytes + sizeof(seed_blk) - 1) / sizeof(seed_blk);
    seed_blk *top = seed_sys_top - units;
    unsigned long a = (unsigned long)top & ~(unsigned long)0xFFF;   // floor address to 4096
    top = (seed_blk *)a;
    if (top < seed_heap + (4u * 1024 * 1024) / sizeof(seed_blk)) return 0;  // keep run room
    seed_sys_top = top;          // seed_heap_init() (lazy, still pending) will honour it
    for (seed_blk *q = top; q < top + units; q++) q->s.next = 0, q->s.size = 0;  // zero
    return top;
}

static void *seed_alloc(unsigned nbytes) {
    if (nbytes == 0) return 0;
    if (!seed_freep) seed_heap_init();
    unsigned need = (nbytes + sizeof(seed_blk) - 1) / sizeof(seed_blk) + 1;  // +header
    seed_blk *prev = seed_freep;
    for (seed_blk *p = prev->s.next; ; prev = p, p = p->s.next) {
        if (p->s.size >= need) {                 // big enough
            if (p->s.size == need) prev->s.next = p->s.next;   // exact: unlink
            else { p->s.size -= need; p += p->s.size; p->s.size = need; }  // carve tail
            seed_freep = prev;
            return (void *)(p + 1);
        }
        if (p == seed_freep) return 0;           // wrapped the whole list: no room
    }
}

static void seed_free(void *ap) {
    if (!ap) return;
    seed_blk *bp = (seed_blk *)ap - 1;
    if (bp < seed_heap || bp >= seed_heap + sizeof(seed_heap) / sizeof(seed_blk))
        return;                                  // ignore a wild pointer
    seed_blk *p = seed_freep;
    while (!(bp > p && bp < p->s.next)) {         // find the insertion point
        if (p >= p->s.next && (bp > p || bp < p->s.next)) break;  // at an end
        p = p->s.next;
    }
    if (bp + bp->s.size == p->s.next) {           // coalesce with the next block
        bp->s.size += p->s.next->s.size;
        bp->s.next  = p->s.next->s.next;
    } else bp->s.next = p->s.next;
    if (p + p->s.size == bp) {                     // coalesce with the previous block
        p->s.size += bp->s.size;
        p->s.next  = bp->s.next;
    } else p->s.next = bp;
    seed_freep = p;
}

// Usable payload bytes of an allocated block (its header records the size).
static unsigned seed_block_bytes(void *ap) {
    seed_blk *bp = (seed_blk *)ap - 1;
    if (bp < seed_heap || bp >= seed_heap + sizeof(seed_heap) / sizeof(seed_blk))
        return 0;
    return (bp->s.size - 1) * (unsigned)sizeof(seed_blk);
}

// realloc: grow or shrink a block, preserving its contents. A shrink (or a grow
// that already fits the rounded-up block) keeps the same pointer; otherwise a new
// block is allocated, the old bytes copied, and the old block freed. On failure
// the original block is left untouched and 0 is returned (standard semantics).
static void *seed_realloc(void *ap, unsigned nbytes) {
    if (!ap) return seed_alloc(nbytes);
    if (nbytes == 0) { seed_free(ap); return 0; }
    unsigned cur = seed_block_bytes(ap);
    if (cur == 0) return 0;                      // wild pointer
    if (nbytes <= cur) return ap;                // already fits
    void *np = seed_alloc(nbytes);
    if (!np) return 0;                           // old block stays valid
    const char *s = ap; char *d = np;
    for (unsigned i = 0; i < cur; i++) d[i] = s[i];
    seed_free(ap);
    return np;
}

// aligned allocation: return a block whose payload is `alignment`-aligned, still
// freeable with the ordinary free. Blocks are already 16-aligned, so smaller
// alignments are plain allocs; for larger ones we over-allocate, split off the
// unaligned prefix as its own free block, and hand back the aligned remainder
// (which carries a normal block header, so free/realloc work on it unchanged).
static void *seed_alloc_aligned(unsigned alignment, unsigned nbytes) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) return 0;  // need pow2
    if (alignment <= sizeof(seed_blk)) return seed_alloc(nbytes);
    void *raw = seed_alloc(nbytes + alignment);
    if (!raw) return 0;
    uintptr_t r = (uintptr_t)raw;
    uintptr_t a = (r + (alignment - 1)) & ~((uintptr_t)alignment - 1);
    if (a == r) return raw;                      // already aligned
    seed_blk *rawh = (seed_blk *)raw - 1;
    unsigned units1 = (unsigned)((a - r) / sizeof(seed_blk));   // unaligned prefix
    seed_blk *ah = rawh + units1;                // header for the aligned block
    ah->s.size   = rawh->s.size - units1;
    rawh->s.size = units1;
    seed_free(raw);                              // return the prefix to the heap
    return (void *)(ah + 1);                     // == a, with a valid header at a-16
}

// ===========================================================================
// Collections: DICT (string-keyed map), LIST (growable sequence), TREE (binary
// search tree keyed by number). Each is a heap object (built on the general
// allocator above, so it is wiped clean on RUN) referred to by a small integer
// handle. Every stored value is tagged number-or-string, so one collection can
// hold either; the $-suffixed words (DICTGET$, POP$, ...) read string values.
// ===========================================================================

// A stored value: a number, or a string copied into the general heap.
typedef struct { int is_str; double num; char *str; int len; } cval_t;

static void cval_clear(cval_t *c) {
    if (c->is_str && c->str) seed_free(c->str);
    c->is_str = 0; c->num = 0; c->str = 0; c->len = 0;
}
// Overwrite *c with the BASIC value v (a string is copied into the heap so it
// survives independently of BASIC's own GC heap). Returns 0 (and raises) on OOM.
static int cval_store(cval_t *c, value_t v) {
    char *p = 0;
    if (v.is_str && v.len > 0) {
        p = (char *)seed_alloc((unsigned)v.len);
        if (!p) { err("Out of memory"); return 0; }
        for (int i = 0; i < v.len; i++) p[i] = v.str[i];
    }
    cval_clear(c);
    if (v.is_str) { c->is_str = 1; c->str = p; c->len = v.len; }
    else          { c->num = v.num; }
    return 1;
}
// Read a stored value as a typed BASIC value. The numeric/string variants raise
// a type mismatch if the stored value is the other kind (like A vs A$).
static value_t cval_num(cval_t *c) {
    if (c->is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
    return v_num(c->num);
}
static value_t cval_strv(cval_t *c) {
    if (!c->is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
    return str_in_scratch(c->str, c->len);
}

// --- LIST: a growable array of values --------------------------------------
typedef struct { cval_t *item; int len, cap; } list_t;

static int list_reserve(list_t *L, int need) {
    if (need <= L->cap) return 1;
    int cap = L->cap ? L->cap * 2 : 8;
    while (cap < need) cap *= 2;
    cval_t *n = (cval_t *)seed_realloc(L->item, (unsigned)(cap * sizeof(cval_t)));
    if (!n) { err("Out of memory"); return 0; }
    L->item = n; L->cap = cap; return 1;
}
static int list_ins(list_t *L, int i, value_t v) {   // insert before index i (0..len)
    if (i < 0 || i > L->len) { err("Index out of range"); return 0; }
    if (!list_reserve(L, L->len + 1)) return 0;
    cval_t tmp = {0, 0, 0, 0};
    if (!cval_store(&tmp, v)) return 0;               // may OOM before we shift
    for (int k = L->len; k > i; k--) L->item[k] = L->item[k - 1];
    L->item[i] = tmp;
    L->len++;
    return 1;
}
static void list_del(list_t *L, int i) {
    if (i < 0 || i >= L->len) { err("Index out of range"); return; }
    cval_clear(&L->item[i]);
    for (int k = i; k < L->len - 1; k++) L->item[k] = L->item[k + 1];
    L->len--;
}

// --- DICT: an insertion-ordered array of (key, value) entries --------------
typedef struct { char *key; int klen; cval_t val; } dent_t;
typedef struct { dent_t *e; int len, cap; } dict_t;

static int dict_find(dict_t *D, const char *key, int klen) {
    for (int i = 0; i < D->len; i++) {
        if (D->e[i].klen != klen) continue;
        int eq = 1;
        for (int k = 0; k < klen; k++) if (D->e[i].key[k] != key[k]) { eq = 0; break; }
        if (eq) return i;
    }
    return -1;
}
static int dict_set(dict_t *D, const char *key, int klen, value_t v) {
    int i = dict_find(D, key, klen);
    if (i >= 0) return cval_store(&D->e[i].val, v);   // update in place
    if (D->len + 1 > D->cap) {
        int cap = D->cap ? D->cap * 2 : 8;
        dent_t *n = (dent_t *)seed_realloc(D->e, (unsigned)(cap * sizeof(dent_t)));
        if (!n) { err("Out of memory"); return 0; }
        D->e = n; D->cap = cap;
    }
    char *kc = 0;
    if (klen > 0) {
        kc = (char *)seed_alloc((unsigned)klen);
        if (!kc) { err("Out of memory"); return 0; }
        for (int k = 0; k < klen; k++) kc[k] = key[k];
    }
    dent_t *e = &D->e[D->len];
    e->key = kc; e->klen = klen;
    e->val.is_str = 0; e->val.num = 0; e->val.str = 0; e->val.len = 0;
    if (!cval_store(&e->val, v)) { if (kc) seed_free(kc); return 0; }
    D->len++;
    return 1;
}
static void dict_del(dict_t *D, const char *key, int klen) {
    int i = dict_find(D, key, klen);
    if (i < 0) return;
    if (D->e[i].key) seed_free(D->e[i].key);
    cval_clear(&D->e[i].val);
    for (int k = i; k < D->len - 1; k++) D->e[k] = D->e[k + 1];
    D->len--;
}

// --- TREE: a binary search tree keyed by number ----------------------------
typedef struct tnode { double key; cval_t val; struct tnode *l, *r; } tnode_t;
typedef struct { tnode_t *root; int count; } tree_t;

static tnode_t *tree_find(tree_t *T, double key) {
    tnode_t *c = T->root;
    while (c) { if (key == c->key) return c; c = key < c->key ? c->l : c->r; }
    return 0;
}
static int tree_set(tree_t *T, double key, value_t v) {
    tnode_t **link = &T->root, *par = 0;
    while (*link) {
        par = *link;
        if (key == par->key) return cval_store(&par->val, v);   // update
        link = key < par->key ? &par->l : &par->r;
    }
    tnode_t *n = (tnode_t *)seed_alloc((unsigned)sizeof(tnode_t));
    if (!n) { err("Out of memory"); return 0; }
    n->key = key; n->l = 0; n->r = 0;
    n->val.is_str = 0; n->val.num = 0; n->val.str = 0; n->val.len = 0;
    if (!cval_store(&n->val, v)) { seed_free(n); return 0; }
    *link = n; T->count++;
    return 1;
}
static void tree_del(tree_t *T, double key) {
    tnode_t *cur = T->root, *par = 0;
    while (cur && cur->key != key) { par = cur; cur = key < cur->key ? cur->l : cur->r; }
    if (!cur) return;                                 // not found
    if (cur->l && cur->r) {                           // two children: use successor
        tnode_t *sp = cur, *s = cur->r;
        while (s->l) { sp = s; s = s->l; }
        cval_clear(&cur->val);
        cur->key = s->key; cur->val = s->val;         // move successor's payload up
        s->val.is_str = 0; s->val.str = 0;            // ...without a double free
        cur = s; par = sp;                            // now splice out s (<=1 child)
    }
    tnode_t *child = cur->l ? cur->l : cur->r;
    if (!par) T->root = child;
    else if (par->l == cur) par->l = child;
    else par->r = child;
    cval_clear(&cur->val);
    seed_free(cur);
    T->count--;
}
static tnode_t *tree_edge(tree_t *T, int rightmost) {  // min (0) or max (1) node
    tnode_t *c = T->root;
    if (!c) return 0;
    while (rightmost ? c->r : c->l) c = rightmost ? c->r : c->l;
    return c;
}
// The idx-th node in ascending key order (0-based). Iterative in-order walk with
// an explicit heap stack, so a degenerate (deep) tree can't overflow the C stack.
static tnode_t *tree_index(tree_t *T, int idx) {
    if (idx < 0 || idx >= T->count) { err("Index out of range"); return 0; }
    tnode_t **stk = (tnode_t **)seed_alloc((unsigned)(T->count * sizeof(tnode_t *)));
    if (!stk) { err("Out of memory"); return 0; }
    int sp = 0, seen = -1;
    tnode_t *cur = T->root, *res = 0;
    while (cur || sp) {
        while (cur) { stk[sp++] = cur; cur = cur->l; }
        cur = stk[--sp];
        if (++seen == idx) { res = cur; break; }
        cur = cur->r;
    }
    seed_free(stk);
    return res;
}

// --- handle pool ------------------------------------------------------------
#define COLL_MAX 64
enum { CT_FREE = 0, CT_DICT, CT_LIST, CT_TREE };
static struct { int type; void *obj; } colls[COLL_MAX];

// Drop every handle. The objects' memory lives in the general heap, which is
// wiped by seed_heap_reset() on RUN/NEW, so we just clear the table alongside it.
static void coll_reset(void) {
    for (int i = 0; i < COLL_MAX; i++) { colls[i].type = CT_FREE; colls[i].obj = 0; }
}
static double coll_new(int type, unsigned objsize) {
    for (int i = 0; i < COLL_MAX; i++) if (colls[i].type == CT_FREE) {
        char *o = (char *)seed_alloc(objsize);
        if (!o) { err("Out of memory"); return 0; }
        for (unsigned k = 0; k < objsize; k++) o[k] = 0;
        colls[i].type = type; colls[i].obj = o;
        return (double)(i + 1);
    }
    err("Too many collections");
    return 0;
}
// Resolve a handle, requiring a particular type (0 = any). Raises on a bad or
// wrong-typed handle and returns 0.
static void *coll_get(double h, int type) {
    int i = (int)h;
    if (i < 1 || i > COLL_MAX || colls[i - 1].type == CT_FREE) { err("Not a collection"); return 0; }
    if (type && colls[i - 1].type != type) {
        err(type == CT_DICT ? "Not a dictionary" : type == CT_LIST ? "Not a list" : "Not a tree");
        return 0;
    }
    return colls[i - 1].obj;
}
static int coll_size_of(int type, void *o) {
    if (type == CT_LIST) return ((list_t *)o)->len;
    if (type == CT_DICT) return ((dict_t *)o)->len;
    if (type == CT_TREE) return ((tree_t *)o)->count;
    return 0;
}

// --- service callbacks the seed may invoke (names are uppercase + suffix) ---
static int bas_getkey(void);                      // GET / INKEY, defined just below:
static int bas_inkey(int cs);                     //   a seed reads the same key stream

static void svc_putc(int c)                       { con_putc((char)c); }
static void svc_puts(const char *s, int len)      { con_putsn(s, len); }
static void svc_vdu(int b)                         { con_vdu(b); }   // VDU control stream
static void svc_screen_size(int *c, int *r)        { if (c) *c = con_cols(); if (r) *r = con_rows(); }
static void svc_con_font(int *w, int *h)           { sgfx_font(w, h); }
static void svc_con_glyph(int px, int py, int ch, uint32_t fg, uint32_t bg) { sgfx_glyph(px, py, ch, fg, bg); }
static int  svc_dir_open(const char *path)         { return stg_diropen(path) == 0 ? 1 : 0; }
static int  svc_dir_read(char *name, int namesz, int *is_dir, long *size) {
    stg_dirent e;
    int r = stg_dirnext(&e);
    if (r != 1) return 0;                                 // 0 = end (or error)
    if (name && namesz > 0) { int i = 0; for (; e.name[i] && i < namesz - 1; i++) name[i] = e.name[i]; name[i] = 0; }
    if (is_dir) *is_dir = e.is_dir;
    if (size)   *size   = e.size;
    return 1;
}
static int  svc_getcwd(char *buf, int sz) {
    const char *c = stg_cwd(); int i = 0;
    if (!buf || sz <= 0) return -1;
    for (; c && c[i] && i < sz - 1; i++) buf[i] = c[i];
    buf[i] = 0;
    return i;
}
static int  svc_chdir(const char *path)            { return stg_chdir(path); }
static void svc_clip_set(const char *d, int n)     { con_clip_set(d, n); }
static int  svc_clip_get(char *b, int max)         { return con_clip_get(b, max); }
static int  svc_clip_len(void)                     { return con_clip_len(); }
static void svc_icache_sync(const void *a, unsigned long n) { icache_sync(a, n); }
static int  svc_getkey(void)                      { return bas_getkey(); }
static int  svc_inkey(int cs)                     { return bas_inkey(cs); }
// The call is the poll: nothing services the mouse while a seed runs, so a seed
// wanting a live pointer calls this in its own loop. Raw framebuffer pixels,
// exactly as BASIC's MOUSEX/MOUSEY and the gfx_* drawing calls see them.
static void svc_mouse(int *x, int *y, int *b)     { con_mouse(x, y, b); }
// Double buffering: the same back buffer (and the same on/off state) BASIC's
// BUFFER/FLIP use, so a seed and a program can't each have their own idea of it.
static int  svc_gfx_backbuffer(int on)            { return con_backbuffer(on); }
static void svc_gfx_flip(void)                    { con_flip(); }
static int  svc_gfx_buffered(void)                { return con_buffered(); }
// Modifier/lock state: not a key, so getkey/inkey can never carry it.
static int  svc_keymods(void)                     { return con_keymods(); }
// The BASIC graphics mode (1 or 2). gfx_* drawing is always device pixels; this
// lets a seed convert coordinates BASIC passed in its current mode.
static int  svc_gfx_mode(void)                    { return con_gfx_mode(); }

// A key that the ON KEY event consumed while detecting the press, held for the
// handler (or the next GET) to read. GET/INKEY go through these wrappers so the
// key that triggered the event is the one the handler reads back.
static int g_pending_key = -1;
static int bas_getkey(void) {
    if (g_pending_key >= 0) { int k = g_pending_key; g_pending_key = -1; return k; }
    return con_getkey();
}
static int bas_inkey(int cs) {
    if (g_pending_key >= 0) { int k = g_pending_key; g_pending_key = -1; return k; }
    return con_inkey(cs);
}

// A record variable has no scalar value of its own, so it is not a number and
// not a string: these four report "not found" for one rather than handing back
// the unused num/s members. Records are reached through the rec_* services.
static int svc_get_num(const char *name, double *out) {
    for (int i = 0; i < var_n; i++)
        if (s_eq(vars[i].name, name) && !vars[i].is_str && !vars[i].is_rec) {
            *out = vars[i].num; return 1;
        }
    *out = 0;
    return 0;
}
static void svc_set_num(const char *name, double val) {
    var_t *v = var_find(name);
    if (v && !v->is_str && !v->is_rec) v->num = trunc_int(v->is_int, val);
}
static int svc_get_str(const char *name, char *buf, int buflen) {
    for (int i = 0; i < var_n; i++)
        if (s_eq(vars[i].name, name) && vars[i].is_str && !vars[i].is_rec) {
            int n = vars[i].s.slen, c = n < buflen ? n : buflen;
            for (int k = 0; k < c; k++) buf[k] = vars[i].s.sptr[k];
            return n;                              // full length, even if truncated
        }
    return 0;
}
static void svc_set_str(const char *name, const char *buf, int len) {
    var_t *v = var_find(name);
    if (v && v->is_str && !v->is_rec) str_store(v, buf, len);
}
static double *svc_num_array(const char *name, int *out_len) {
    arr_t *a = arr_find(name);
    if (!a || a->is_str) { if (out_len) *out_len = 0; return 0; }
    if (out_len) *out_len = a->total;
    return &arr_nums[a->off];                      // pool never moves: safe to hand out
}

// --- records ----------------------------------------------------------------
// The same zero-copy bargain as num_array, for TYPE records: an element's
// numeric fields are contiguous, so a record array is a strided double array.

static const var_t *svc_rec_find(const char *name) {
    const var_t *v = var_lookup(name);
    return (v && v->is_rec) ? v : 0;
}

static double *svc_rec_array(const char *name, int *nelem, int *stride) {
    const var_t *v = svc_rec_find(name);
    if (!v) { if (nelem) *nelem = 0; if (stride) *stride = 0; return 0; }
    if (nelem)  *nelem  = v->nelem;
    if (stride) *stride = types[v->rtype].n_num;
    return &rec_nums[v->roff_num];                 // pool never moves
}

static int svc_rec_field(const char *name, const char *field) {
    const var_t *v = svc_rec_find(name);
    if (!v) return -1;
    const type_t *ty = &types[v->rtype];
    int f = type_field_in(ty, field);
    if (f < 0 || ty->fis_str[f]) return -1;        // text fields aren't in the block
    return ty->fslot[f];
}

// Resolve a text field to its descriptor; 0 if the record/element/field is not
// one. Shared by the two string services below.
static strdesc_t *svc_rec_str_slot(const char *name, int elem, const char *field) {
    const var_t *v = svc_rec_find(name);
    if (!v || elem < 0 || elem >= v->nelem) return 0;
    const type_t *ty = &types[v->rtype];
    int f = type_field_in(ty, field);
    if (f < 0 || !ty->fis_str[f]) return 0;
    return &rec_strs[rec_str_slot(v, elem, f)];
}

static int svc_rec_get_str(const char *name, int elem, const char *field,
                           char *buf, int buflen) {
    const strdesc_t *d = svc_rec_str_slot(name, elem, field);
    if (!d) return 0;
    int n = d->slen, c = n < buflen ? n : buflen;
    for (int k = 0; k < c; k++) buf[k] = d->sptr[k];
    return n;                                      // full length, even if truncated
}

static void svc_rec_set_str(const char *name, int elem, const char *field,
                            const char *buf, int len) {
    strdesc_t *d = svc_rec_str_slot(name, elem, field);
    if (d) str_store_to(d, buf, len);
}
static void svc_set_return_str(const char *buf, int len) {
    if (len > MAX_STR) len = MAX_STR;
    for (int i = 0; i < len; i++) g_seed_retstr[i] = buf[i];
    g_seed_retstr_len = len;
}
static uint32_t svc_time_cs(void) { return (uint32_t)(con_micros() / 10000ULL); }
static void *svc_alloc(unsigned nbytes) { return seed_alloc(nbytes); }
static void  svc_free(void *ptr)        { seed_free(ptr); }
static void *svc_realloc(void *ptr, unsigned nbytes) { return seed_realloc(ptr, nbytes); }
static void *svc_alloc_aligned(unsigned a, unsigned n) { return seed_alloc_aligned(a, n); }

// GPIO passthroughs (see gpio.h). The driver validates the pin range itself, so
// these are thin; on the host build every gpio_* is a stub and gpio_available()
// is 0, which a seed can test via svc->gpio_avail().
static int  svc_gpio_avail(void)                       { return gpio_available(); }
static int  svc_gpio_mode(int pin, int mode, int alt)  { return gpio_set_mode(pin, mode, alt); }
static int  svc_gpio_pull(int pin, int pull)           { return gpio_set_pull(pin, pull); }
static void svc_gpio_write(int pin, int level)         { gpio_write(pin, level); }
static int  svc_gpio_read(int pin)                     { return gpio_read(pin); }
static void svc_gpio_set(uint32_t mask)                { gpio_set_mask(mask); }
static void svc_gpio_clr(uint32_t mask)                { gpio_clr_mask(mask); }
static uint32_t svc_gpio_level(void)                   { return gpio_read_all(); }
static int  svc_gpio_wait(int pin, int edge, int cs)   { return gpio_wait_edge(pin, edge, cs); }

// SD-card files: thin adapters over the storage channel API (see storage.h),
// which the seed <stdio.h> is built on. All of this shares the file channels and
// filesystem (long names included) with BASIC's OPENIN/OPENOUT.
static int svc_file_open(const char *name, int mode) {
    int m = (mode == SEED_FOPEN_WRITE)  ? STG_M_WRITE
          : (mode == SEED_FOPEN_UPDATE) ? STG_M_UPDATE : STG_M_READ;
    return stg_open(name, m);                         // channel > 0, or 0 on failure
}
static int svc_file_close(int fh) { return stg_close(fh); }
static int svc_file_read(int fh, void *buf, int n) {
    unsigned char *p = buf;
    int i = 0;
    for (; i < n; i++) { int b = stg_getb(fh); if (b < 0) break; p[i] = (unsigned char)b; }
    return i;                                         // short count at EOF (stdio semantics)
}
static int svc_file_write(int fh, const void *buf, int n) {
    const unsigned char *p = buf;
    for (int i = 0; i < n; i++) { int r = stg_putb(fh, p[i]); if (r < 0) return i > 0 ? i : r; }
    return n;
}
static long svc_file_seek(int fh, long off, int whence) {
    long base = (whence == 1) ? stg_tell(fh) : (whence == 2) ? stg_size(fh) : 0;
    if (base < 0) return base;
    long pos = base + off;
    int r = stg_seek(fh, pos);
    return r < 0 ? (long)r : pos;
}
static long svc_file_size(int fh)   { return stg_size(fh); }
static int  svc_file_eof(int fh)    { return stg_eof(fh); }
static int  svc_file_remove(const char *name) { return stg_delete(name); }

// Format a double exactly as PRINT/STR$ do, so a seed's printf %f matches BASIC.
static int svc_fmt_num(double v, char *out) { return dbl_to_str(out, v); }

// --- graphics + TrueType text for seeds (ABI v7) ---------------------------
// Device-pixel drawing forwards to gfx.h (framebuffer target / host no-op);
// font management reuses the same ttf.h engine as BASIC's GTEXT.
static int  svc_gfx_avail(void)  { return sgfx_avail(); }
static int  svc_gfx_width(void)  { return sgfx_width(); }
static int  svc_gfx_height(void) { return sgfx_height(); }
static void svc_gfx_clear(uint32_t rgb) { sgfx_clear(rgb); }
static void svc_gfx_putpixel(int x, int y, uint32_t rgb) { sgfx_putpixel(x, y, rgb); }
static uint32_t svc_gfx_getpixel(int x, int y) { return sgfx_getpixel(x, y); }
static void svc_gfx_line(int x1, int y1, int x2, int y2, uint32_t rgb) { sgfx_line(x1, y1, x2, y2, rgb); }
static void svc_gfx_fillrect(int x1, int y1, int x2, int y2, uint32_t rgb) { sgfx_fillrect(x1, y1, x2, y2, rgb); }
static void svc_gfx_circle(int cx, int cy, int r, uint32_t rgb) { sgfx_circle(cx, cy, r, rgb); }
static void svc_gfx_fillcircle(int cx, int cy, int r, uint32_t rgb) { sgfx_fillcircle(cx, cy, r, rgb); }
static void svc_gfx_ellipse(int cx, int cy, int rx, int ry, uint32_t rgb) { sgfx_ellipse(cx, cy, rx, ry, rgb); }
static void svc_gfx_fillellipse(int cx, int cy, int rx, int ry, uint32_t rgb) { sgfx_fillellipse(cx, cy, rx, ry, rgb); }
static void svc_gfx_fillpoly(const int *xy, int npts, uint32_t rgb) { sgfx_fillpoly(xy, npts, rgb); }
static void svc_gfx_line_style(int w, int j, int c) { sgfx_line_style(w, j, c); }
static void svc_gfx_polyline(const int *pts, int n, int cl, uint32_t rgb) { sgfx_polyline(pts, n, cl, rgb); }
static void svc_gfx_flood(int x, int y, uint32_t rgb) { sgfx_flood(x, y, rgb); }
static void svc_gfx_clip(int x1, int y1, int x2, int y2) { sgfx_clip(x1, y1, x2, y2); }
static void svc_gfx_noclip(void) { sgfx_noclip(); }
static int  svc_font_load(const char *name) { return ttf_load(name); }
static int  svc_font_select(int handle) { return ttf_select(handle); }
static void svc_font_size(int px) { ttf_set_size(px); }
static void svc_font_style(int b, int i, int u) { ttf_set_style(b, i, u); }
static void svc_gfx_text(int x, int y, const char *s, int len, uint32_t rgb) { sgfx_text(x, y, s, len, rgb); }
static int  svc_text_width(const char *s, int len) { return ttf_text_width(s, len); }
static int  svc_text_height(void) { return ttf_line_height(); }

static const BerryServices g_svc = {
    BERRY_ABI_VERSION,
    svc_putc, svc_puts, svc_getkey, svc_inkey,
    svc_get_num, svc_set_num, svc_get_str, svc_set_str,
    svc_num_array, svc_set_return_str, svc_time_cs,
    svc_alloc, svc_free,
    svc_realloc, svc_alloc_aligned,
    svc_gpio_avail, svc_gpio_mode, svc_gpio_pull, svc_gpio_write, svc_gpio_read,
    svc_gpio_set, svc_gpio_clr, svc_gpio_level, svc_gpio_wait,
    svc_file_open, svc_file_close, svc_file_read, svc_file_write,
    svc_file_seek, svc_file_size, svc_file_eof, svc_file_remove,
    svc_fmt_num,
    svc_gfx_avail, svc_gfx_width, svc_gfx_height, svc_gfx_clear,
    svc_gfx_putpixel, svc_gfx_getpixel, svc_gfx_line, svc_gfx_fillrect,
    svc_gfx_circle, svc_gfx_fillcircle, svc_gfx_ellipse, svc_gfx_fillellipse,
    svc_gfx_fillpoly, svc_gfx_flood, svc_gfx_clip, svc_gfx_noclip,
    svc_font_load, svc_font_select, svc_font_size, svc_font_style,
    svc_gfx_text, svc_text_width, svc_text_height,
    svc_rec_array, svc_rec_field, svc_rec_get_str, svc_rec_set_str,
    svc_mouse,
    svc_gfx_backbuffer, svc_gfx_flip, svc_gfx_buffered,
    svc_keymods,
    svc_gfx_mode,
    svc_gfx_line_style, svc_gfx_polyline,
    svc_vdu, svc_screen_size, svc_con_font, svc_con_glyph,
    svc_dir_open, svc_dir_read, svc_getcwd, svc_chdir,
    svc_clip_set, svc_clip_get, svc_clip_len,
    svc_icache_sync,
};

// Parse "handle [, arg ...]" (tok at the handle, stops on the first non-comma
// token), invoke the seed, and return its numeric result. String arguments are
// snapshotted into scratch so they stay valid even if the seed triggers GC by
// writing a variable. g_seed_retstr[_len] receives any string result.
static double seed_run_collect(void) {
    double h = need_num();
    if (g_err) return 0;
    int slot = (int)h - 1;
    if (slot < 0 || slot >= seed_rec_n || !seed_recs[slot].used) { err("No such seed"); return 0; }

    berry_arg argv[SEED_MAX_ARGS];
    int argc = 0;
    while (tok == T_COMMA) {
        lex_next();
        if (argc >= SEED_MAX_ARGS) { err("Too many arguments"); return 0; }
        value_t v = eval_expr();
        if (g_err) return 0;
        if (v.is_str) {
            value_t snap = str_in_scratch(v.str, v.len);   // GC-stable copy
            if (g_err) return 0;
            argv[argc].is_str = 1; argv[argc].num = 0;
            argv[argc].str = snap.str; argv[argc].len = snap.len;
        } else {
            argv[argc].is_str = 0; argv[argc].num = v.num;
            argv[argc].str = 0; argv[argc].len = 0;
        }
        argc++;
    }

    g_seed_retstr_len = -1;
    double ret = 0;
    if (seed_invoke(seed_recs[slot].entry, &g_svc, argv, argc, &ret) != 0) {
        err("Native seeds run on the Pi, not the host build");
        return 0;
    }
    return ret;
}


// ---------------------------------------------------------------------------
// Seed-registered keywords: seeds that carry a seed_keyword descriptor (built
// with SEED_KEYWORD) extend the language with a new command or function. They
// are scanned from /seed once at startup (seed_scan_keywords, in the storage
// section) into a resident pool that a RUN/NEW never clears, so the keyword is
// always available — used directly, without SEED/CALL.
// ---------------------------------------------------------------------------
// Native keyword seeds are scanned once at startup, so their code is carved from
// the persistent top of the seed heap (seed_persist_page) and survives every
// RUN/NEW; there is no per-seed size limit. The registry table below holds both
// native seeds and POD extensions. It is a fixed, generous size: keyword
// extensions are inherently few (the whole language has ~100 keywords), and
// unlike the seed code it must persist across RUN yet grow at PODLOAD time, which
// a top-of-heap bump cannot do safely once the run heap is live.
#define SEED_KW_MAX 256

// A dynamic (runtime-registered) keyword. Both native seeds (from /seed at
// startup) and POD extensions (PODLOAD) live in this one table, so the lexer and
// dispatch treat them alike. is_pod picks the backend: a native seed runs from
// its persistent code block via `entry`; a POD keyword runs from pod_entry
// (inside a resident POD image) through its own capability-gated pod_svc.
typedef struct {
    char name[16]; int kind; int minargs; int maxargs;
    int is_pod;              // 0 = native seed, 1 = POD keyword
    int slot;               // POD: pod_pool index (PODFREE drops its keywords). native: -1
    const void *entry;      // native seed: entry pointer into its persistent block
    const void *pod_entry, *pod_svc;
} seed_kw_t;

static seed_kw_t  seed_kw_tab[SEED_KW_MAX];
static int        seed_kw_n = 0;

// Find a registered keyword by (already upper-cased) name; -1 if none.
static int seed_kw_lookup(const char *name) {
    for (int i = 0; i < seed_kw_n; i++)
        if (s_eq(name, seed_kw_tab[i].name)) return i;
    return -1;
}

// The spelling of a seed's keyword, by registry index. The counterpart of
// kw_spelling() for built-ins: a type or field may be named after a keyword,
// and a seed-registered one is no different (a seed adding PARTICLE must not
// stop you writing TYPE particle).
static const char *seed_kw_name(int idx) {
    if (idx < 0 || idx >= seed_kw_n) return 0;
    return seed_kw_tab[idx].name;
}

// Bytes to reserve for a loaded seed: the header's image_size (total footprint
// including .bss, stamped by tcc -seed) when it is known, else the blob length
// plus a 16 KB zeroed margin for a gcc-built seed's .bss (matching the old fixed
// slot, which zeroed 16 KB). Rounded up to a page.
static unsigned seed_footprint(const struct seed_header *hdr, int len) {
    unsigned foot = hdr->image_size ? hdr->image_size : (unsigned)len + 16384u;
    if (foot < (unsigned)len) foot = (unsigned)len;
    return (foot + 4095u) & ~4095u;
}

// Validate a loaded .sed blob and, if it registers a keyword, copy it into a
// persistent code block and add it to the table. Returns 1 if a keyword was
// installed.
static int seed_kw_register(const char *blob, int len) {
    if (len < (int)(sizeof(struct seed_header) + sizeof(struct seed_keyword))) return 0;
    struct seed_header hdr;                         // copy bytewise (-mstrict-align)
    for (int i = 0; i < (int)sizeof(hdr); i++) ((char *)&hdr)[i] = blob[i];
    if (hdr.magic != SEED_MAGIC)             return 0;
    if (hdr.version > BERRY_ABI_VERSION)      return 0;
    if (!(hdr.flags & SEED_HDR_KEYWORD))     return 0;   // a plain seed: not a keyword
    if (hdr.entry_off >= (uint32_t)len)      return 0;
    if (seed_kw_n >= SEED_KW_MAX)            return 0;   // registry full

    struct seed_keyword d;
    for (int i = 0; i < (int)sizeof(d); i++)
        ((char *)&d)[i] = blob[sizeof(struct seed_header) + i];

    char name[16]; int nlen = 0;                    // uppercase, NUL-terminate, keep '$'
    for (int i = 0; i < 15 && d.name[i]; i++) name[nlen++] = up(d.name[i]);
    name[nlen] = 0;
    if (nlen == 0)                 return 0;         // empty name
    if (seed_kw_lookup(name) >= 0) return 0;         // already registered

    int kind = d.kind; if (kind < 0 || kind > SEED_KW_STRFN) kind = SEED_KW_STATEMENT;
    int mn = d.min_args, mx = d.max_args;
    if (mn > SEED_MAX_ARGS) mn = SEED_MAX_ARGS;
    if (mx > SEED_MAX_ARGS) mx = SEED_MAX_ARGS;
    if (mx < mn)            mx = mn;

    char *mem = (char *)seed_persist_page(seed_footprint(&hdr, len));   // zeroed, page-aligned
    if (!mem) return 0;                                // out of persistent room
    for (int i = 0; i < len; i++) mem[i] = blob[i];
    icache_sync(mem, (unsigned long)len);              // make executable

    int slot = seed_kw_n;
    s_copy(seed_kw_tab[slot].name, name, 16);
    seed_kw_tab[slot].kind    = kind;
    seed_kw_tab[slot].minargs = mn;
    seed_kw_tab[slot].maxargs = mx;
    seed_kw_tab[slot].is_pod  = 0;              // a native seed keyword
    seed_kw_tab[slot].slot    = -1;             // native: not a POD slot
    seed_kw_tab[slot].entry   = (const void *)(mem + hdr.entry_off);
    seed_kw_tab[slot].pod_entry = 0;
    seed_kw_tab[slot].pod_svc   = 0;
    seed_kw_n++;
    return 1;
}

// Snapshot one evaluated value into a berry_arg (strings copied to GC-stable
// scratch, valid for the duration of the call).
static int seed_fill_arg(berry_arg *a, value_t v) {
    if (v.is_str) {
        value_t snap = str_in_scratch(v.str, v.len);
        if (g_err) return 0;
        a->is_str = 1; a->num = 0; a->str = snap.str; a->len = snap.len;
    } else {
        a->is_str = 0; a->num = v.num; a->str = 0; a->len = 0;
    }
    return 1;
}

// Gather the argument list for a seed keyword. paren=1 reads NAME(a, b, ...);
// paren=0 reads the bare statement form NAME a, b, ... up to end of statement.
// Returns the count, or -1 on error (with g_err set).
static int seed_gather_args(berry_arg *argv, int max, int paren) {
    int argc = 0;
    if (paren) {
        if (!expect(T_LP)) return -1;
        if (tok == T_RP) { lex_next(); return 0; }        // NAME()
    } else if (tok == T_EOL || tok == T_COLON) {
        return 0;                                          // NAME (no args)
    }
    for (;;) {
        if (argc >= max) { err("Too many arguments"); return -1; }
        value_t v = eval_expr(); if (g_err) return -1;
        if (!seed_fill_arg(&argv[argc], v)) return -1;
        argc++;
        if (tok == T_COMMA) { lex_next(); continue; }
        break;
    }
    if (paren && !expect(T_RP)) return -1;
    return argc;
}

// Invoke a registered keyword's seed with the gathered args. Returns its numeric
// result in *out (string results are staged in g_seed_retstr). 0 ok, -1 on error.
static int seed_kw_invoke(seed_kw_t *k, berry_arg *argv, int argc, double *out) {
    if (argc < k->minargs || argc > k->maxargs) { err("Wrong number of arguments"); return -1; }
    g_seed_retstr_len = -1;
    *out = 0;
    int rc;
    if (k->is_pod)          // a POD keyword: its own image + capability-gated table
        rc = pod_run_kw(k->pod_entry, (const BerryServices *)k->pod_svc,
                        (const berry_arg *)argv, argc, out);
    else                    // a native seed keyword
        rc = seed_invoke((seed_entry)k->entry, &g_svc, argv, argc, out);
    if (rc != 0) { err("Native code runs on the Pi, not the host build"); return -1; }
    return 0;
}

// Function form: x = NAME(args) / a$ = NAME$(args). Current token is the keyword.
static value_t eval_seed_keyword(int id) {
    seed_kw_t *k = &seed_kw_tab[id - KW_SEED_DYN];
    lex_next();                                            // consume the keyword
    berry_arg argv[SEED_MAX_ARGS];
    int argc = seed_gather_args(argv, SEED_MAX_ARGS, 1);
    if (argc < 0) return v_num(0);
    double ret;
    if (seed_kw_invoke(k, argv, argc, &ret) != 0) return v_num(0);
    if (k->kind == SEED_KW_STRFN)
        return (g_seed_retstr_len >= 0) ? str_in_scratch(g_seed_retstr, g_seed_retstr_len)
                                        : str_in_scratch("", 0);
    return v_num(ret);
}

// Statement form: NAME arg, arg. Current token is the keyword; result discarded.
static void exec_seed_keyword(int id) {
    seed_kw_t *k = &seed_kw_tab[id - KW_SEED_DYN];
    lex_next();                                            // consume the keyword
    berry_arg argv[SEED_MAX_ARGS];
    int argc = seed_gather_args(argv, SEED_MAX_ARGS, 0);
    if (argc < 0) return;
    double ret;
    seed_kw_invoke(k, argv, argc, &ret);
}
