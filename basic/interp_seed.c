#include "interp_seed.h"
#include "interp_util.h"
#include "interp_data.h"
#include "interp_lexer.h"
#include "interp_parse.h"
#include "interp_eval.h"
#include "interp_stmt.h"
#include "interp_files.h"
#include "interp_pod.h"
#include "interp_control.h"
// ===========================================================================
// BerryBasiC — native seeds: heap, collections (DICT/LIST/TREE), seed-service vtable
//
// A separately-compiled module of the interpreter. Its cross-module
// interface is declared in interp_seed.h (extern globals + documented function
// prototypes) and interp_types.h (the shared data types).
// ===========================================================================
// ---------------------------------------------------------------------------
// Native seeds: small chunks of position-independent AArch64 machine code that
// a program loads (SEED) and calls (CALL / CALL$). The blob is copied into a
// page-aligned, executable RAM slot; seeds reach the interpreter only through
// the BerryServices vtable below, which is what keeps them self-contained. See
// seed/seed.h for the ABI.
// ---------------------------------------------------------------------------

// A loaded SEED/CALL seed. There is no fixed number of them and no fixed size:
// each seed's code is a page-aligned block carved from the run heap and sized to
// the blob, and the record table grows on demand. Both are wiped on RUN/NEW (a
// program reloads its own seeds), so seed_recs is reset to 0 there. Handle =
// record index + 1.
seed_rec_t *seed_recs;          // grown from the run heap; 0 after a reset
int         seed_rec_n;         // records touched (high-water within the array)
int         seed_rec_cap;       // allocated capacity

void *seed_alloc(unsigned nbytes);   // the run-heap allocator, defined below

// Drop every loaded seed. The blobs and this table live in the run heap, which
// seed_heap_reset() wipes on RUN/NEW, so we just forget the pointers.
void seed_recs_reset(void) { seed_recs = 0; seed_rec_n = 0; seed_rec_cap = 0; }

// Reserve a fresh record and return its index, or -1 on OOM. Reuses a freed slot
// when one exists, else appends (growing the array from the run heap).
int seed_rec_alloc(void) {
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
void *seed_alloc_page(unsigned nbytes) {
    unsigned asz = (nbytes + 4095u) & ~4095u;
    char *raw = (char *)seed_alloc(asz + 4096u);        // slack to reach a page boundary
    if (!raw) return 0;
    return (void *)(((unsigned long)raw + 4095u) & ~(unsigned long)4095u);
}

char g_seed_retstr[MAX_STR];   // string result staged by set_return_str
int  g_seed_retstr_len;        // -1 = the last call set no string

// --- seed heap -------------------------------------------------------------
// A general-purpose allocator for seeds (BASIC's own string/array storage is
// separate). Classic K&R first-fit over a fixed arena with coalescing on free;
// returns 0 when exhausted. The whole arena is reclaimed at each RUN/NEW, so a
// seed that forgets to free leaks only within the current run. 16-byte aligned
// blocks, suitable for doubles and NEON.
                                                // tcc-as-a-POD needs elbow room to compile


// 4096-aligned so a page-aligned sub-allocation is a whole number of blocks from
// the base (native seed code needs a page-aligned load address for ADRP+ADD).
seed_blk seed_heap[SEED_HEAP_SIZE / sizeof(seed_blk)] __attribute__((aligned(4096)));
seed_blk seed_freelist;        // circular free-list sentinel
seed_blk *seed_freep;          // 0 until first use / after a reset

// Session-persistent high-water mark. Native seed code (both SEED/CALL blobs and
// keyword seeds) has to run from executable RAM; there is no OS page allocator,
// so it is carved from this same 48 MB arena. Keyword seeds, however, must SURVIVE
// RUN/NEW (they are language extensions loaded once at startup), and the run heap
// below is wiped on every RUN. So keyword-seed code and the keyword table grow
// DOWN from the top of the arena, past seed_sys_top, which the reset leaves alone;
// the K&R free-list only ever covers [seed_heap, seed_sys_top). When no keyword
// seeds are loaded, seed_sys_top is the arena end and the heap behaves as before.
seed_blk *seed_sys_top;        // 0 until first use; else the persistent floor

void seed_heap_reset(void) { seed_freep = 0; }     // lazily re-inited

// Report the run heap's used/total bytes (for the F12 system overlay). Total is
// the run-heap region [seed_heap, seed_sys_top); free is the sum of the K&R
// free-list blocks (before first use the whole region is free). Sizes are in
// header units; ×sizeof(seed_blk) gives bytes.
void seed_heap_stats(unsigned long *used, unsigned long *total) {
    unsigned long tot = seed_sys_top
        ? (unsigned long)(seed_sys_top - seed_heap)
        : (unsigned long)(sizeof(seed_heap) / sizeof(seed_blk));
    unsigned long freeu;
    if (!seed_freep) {
        freeu = tot;                                     // not yet used: all free
    } else {
        freeu = 0;
        for (seed_blk *p = seed_freelist.s.next; p != &seed_freelist; p = p->s.next)
            freeu += p->s.size;                          // sentinel size is 0, stops the walk
    }
    if (freeu > tot) freeu = tot;
    if (total) *total = tot * (unsigned long)sizeof(seed_blk);
    if (used)  *used  = (tot - freeu) * (unsigned long)sizeof(seed_blk);
}

// Exported (see console.h): fill the interpreter's memory-pool usage for the
// kernel's F12 overlay. Non-static on purpose - this is the one symbol the
// kernel reaches into the interpreter for. Harmless (unused) on the host build.
void sys_meminfo(sysmem_t *m) {
    if (!m) return;
    seed_heap_stats(&m->heap_used, &m->heap_total);
    m->vars_used  = (unsigned long)gcheap_top;
    m->vars_total = (unsigned long)GCHEAP_SIZE;
    m->dim_used   = (unsigned long)dim_top;
    m->dim_total  = (unsigned long)DIM_HEAP_SIZE;
}

void seed_heap_init(void) {
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
void *seed_persist_page(unsigned nbytes) {
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

void *seed_alloc(unsigned nbytes) {
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

void seed_free(void *ap) {
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
unsigned seed_block_bytes(void *ap) {
    seed_blk *bp = (seed_blk *)ap - 1;
    if (bp < seed_heap || bp >= seed_heap + sizeof(seed_heap) / sizeof(seed_blk))
        return 0;
    return (bp->s.size - 1) * (unsigned)sizeof(seed_blk);
}

// realloc: grow or shrink a block, preserving its contents. A shrink (or a grow
// that already fits the rounded-up block) keeps the same pointer; otherwise a new
// block is allocated, the old bytes copied, and the old block freed. On failure
// the original block is left untouched and 0 is returned (standard semantics).
void *seed_realloc(void *ap, unsigned nbytes) {
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
void *seed_alloc_aligned(unsigned alignment, unsigned nbytes) {
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

void cval_clear(cval_t *c) {
    if (c->is_str && c->str) seed_free(c->str);
    c->is_str = 0; c->num = 0; c->str = 0; c->len = 0;
}
// Overwrite *c with the BASIC value v (a string is copied into the heap so it
// survives independently of BASIC's own GC heap). Returns 0 (and raises) on OOM.
int cval_store(cval_t *c, value_t v) {
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
value_t cval_num(cval_t *c) {
    if (c->is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
    return v_num(c->num);
}
value_t cval_strv(cval_t *c) {
    if (!c->is_str) { err("Type mismatch: numbers and text can't be mixed"); return v_num(0); }
    return str_in_scratch(c->str, c->len);
}

// --- LIST: a growable array of values --------------------------------------

int list_reserve(list_t *L, int need) {
    if (need <= L->cap) return 1;
    int cap = L->cap ? L->cap * 2 : 8;
    while (cap < need) cap *= 2;
    cval_t *n = (cval_t *)seed_realloc(L->item, (unsigned)(cap * sizeof(cval_t)));
    if (!n) { err("Out of memory"); return 0; }
    L->item = n; L->cap = cap; return 1;
}
int list_ins(list_t *L, int i, value_t v) {   // insert before index i (0..len)
    if (i < 0 || i > L->len) { err("Index out of range"); return 0; }
    if (!list_reserve(L, L->len + 1)) return 0;
    cval_t tmp = {0, 0, 0, 0};
    if (!cval_store(&tmp, v)) return 0;               // may OOM before we shift
    for (int k = L->len; k > i; k--) L->item[k] = L->item[k - 1];
    L->item[i] = tmp;
    L->len++;
    return 1;
}
void list_del(list_t *L, int i) {
    if (i < 0 || i >= L->len) { err("Index out of range"); return; }
    cval_clear(&L->item[i]);
    for (int k = i; k < L->len - 1; k++) L->item[k] = L->item[k + 1];
    L->len--;
}

// --- DICT: an insertion-ordered array of (key, value) entries --------------

int dict_find(dict_t *D, const char *key, int klen) {
    for (int i = 0; i < D->len; i++) {
        if (D->e[i].klen != klen) continue;
        int eq = 1;
        for (int k = 0; k < klen; k++) if (D->e[i].key[k] != key[k]) { eq = 0; break; }
        if (eq) return i;
    }
    return -1;
}
int dict_set(dict_t *D, const char *key, int klen, value_t v) {
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
void dict_del(dict_t *D, const char *key, int klen) {
    int i = dict_find(D, key, klen);
    if (i < 0) return;
    if (D->e[i].key) seed_free(D->e[i].key);
    cval_clear(&D->e[i].val);
    for (int k = i; k < D->len - 1; k++) D->e[k] = D->e[k + 1];
    D->len--;
}

// --- TREE: a binary search tree keyed by number ----------------------------

tnode_t *tree_find(tree_t *T, double key) {
    tnode_t *c = T->root;
    while (c) { if (key == c->key) return c; c = key < c->key ? c->l : c->r; }
    return 0;
}
int tree_set(tree_t *T, double key, value_t v) {
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
void tree_del(tree_t *T, double key) {
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
tnode_t *tree_edge(tree_t *T, int rightmost) {  // min (0) or max (1) node
    tnode_t *c = T->root;
    if (!c) return 0;
    while (rightmost ? c->r : c->l) c = rightmost ? c->r : c->l;
    return c;
}
// The idx-th node in ascending key order (0-based). Iterative in-order walk with
// an explicit heap stack, so a degenerate (deep) tree can't overflow the C stack.
tnode_t *tree_index(tree_t *T, int idx) {
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
coll_t colls[COLL_MAX];

// Drop every handle. The objects' memory lives in the general heap, which is
// wiped by seed_heap_reset() on RUN/NEW, so we just clear the table alongside it.
void coll_reset(void) {
    for (int i = 0; i < COLL_MAX; i++) { colls[i].type = CT_FREE; colls[i].obj = 0; }
}
double coll_new(int type, unsigned objsize) {
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
void *coll_get(double h, int type) {
    int i = (int)h;
    if (i < 1 || i > COLL_MAX || colls[i - 1].type == CT_FREE) { err("Not a collection"); return 0; }
    if (type && colls[i - 1].type != type) {
        err(type == CT_DICT ? "Not a dictionary" : type == CT_LIST ? "Not a list" : "Not a tree");
        return 0;
    }
    return colls[i - 1].obj;
}
int coll_size_of(int type, void *o) {
    if (type == CT_LIST) return ((list_t *)o)->len;
    if (type == CT_DICT) return ((dict_t *)o)->len;
    if (type == CT_TREE) return ((tree_t *)o)->count;
    return 0;
}

// --- service callbacks the seed may invoke (names are uppercase + suffix) ---
int bas_getkey(void);                      // GET / INKEY, defined just below:
int bas_inkey(int cs);                     //   a seed reads the same key stream

void svc_putc(int c)                       { con_putc((char)c); }
void svc_puts(const char *s, int len)      { con_putsn(s, len); }
void svc_vdu(int b)                         { con_vdu(b); }   // VDU control stream
void svc_screen_size(int *c, int *r)        { if (c) *c = con_cols(); if (r) *r = con_rows(); }
void svc_con_font(int *w, int *h)           { sgfx_font(w, h); }
void svc_con_glyph(int px, int py, int ch, uint32_t fg, uint32_t bg) { sgfx_glyph(px, py, ch, fg, bg); }
int  svc_dir_open(const char *path)         { return stg_diropen(path) == 0 ? 1 : 0; }
int  svc_dir_read(char *name, int namesz, int *is_dir, long *size) {
    stg_dirent e;
    int r = stg_dirnext(&e);
    if (r != 1) return 0;                                 // 0 = end (or error)
    if (name && namesz > 0) { int i = 0; for (; e.name[i] && i < namesz - 1; i++) name[i] = e.name[i]; name[i] = 0; }
    if (is_dir) *is_dir = e.is_dir;
    if (size)   *size   = e.size;
    return 1;
}
int  svc_getcwd(char *buf, int sz) {
    const char *c = stg_cwd(); int i = 0;
    if (!buf || sz <= 0) return -1;
    for (; c && c[i] && i < sz - 1; i++) buf[i] = c[i];
    buf[i] = 0;
    return i;
}
int  svc_chdir(const char *path)            { return stg_chdir(path); }
void svc_clip_set(const char *d, int n)     { con_clip_set(d, n); }
int  svc_clip_get(char *b, int max)         { return con_clip_get(b, max); }
int  svc_clip_len(void)                     { return con_clip_len(); }
void svc_icache_sync(const void *a, unsigned long n) { icache_sync(a, n); }
int  svc_getkey(void)                      { return bas_getkey(); }
int  svc_inkey(int cs)                     { return bas_inkey(cs); }
// The call is the poll: nothing services the mouse while a seed runs, so a seed
// wanting a live pointer calls this in its own loop. Raw framebuffer pixels,
// exactly as BASIC's MOUSEX/MOUSEY and the gfx_* drawing calls see them.
void svc_mouse(int *x, int *y, int *b)     { con_mouse(x, y, b); }
// Double buffering: the same back buffer (and the same on/off state) BASIC's
// BUFFER/FLIP use, so a seed and a program can't each have their own idea of it.
int  svc_gfx_backbuffer(int on)            { return con_backbuffer(on); }
void svc_gfx_flip(void)                    { con_flip(); }
int  svc_gfx_buffered(void)                { return con_buffered(); }
// Modifier/lock state: not a key, so getkey/inkey can never carry it.
int  svc_keymods(void)                     { return con_keymods(); }
int  svc_keys_down(int *out, int max)      { return con_keys_down(out, max); }
int  svc_audio_open(int rate)              { return snd_pcm_open(rate); }
int  svc_audio_avail(void)                 { return snd_pcm_avail(); }
int  svc_audio_write(const short *s, int f){ return snd_pcm_write(s, f); }
void svc_audio_close(void)                 { snd_pcm_close(); }
// The BASIC graphics mode (1 or 2). gfx_* drawing is always device pixels; this
// lets a seed convert coordinates BASIC passed in its current mode.
int  svc_gfx_mode(void)                    { return con_gfx_mode(); }

// A key that the ON KEY event consumed while detecting the press, held for the
// handler (or the next GET) to read. GET/INKEY go through these wrappers so the
// key that triggered the event is the one the handler reads back.
int g_pending_key = -1;
int bas_getkey(void) {
    if (g_pending_key >= 0) { int k = g_pending_key; g_pending_key = -1; return k; }
    return con_getkey();
}
int bas_inkey(int cs) {
    if (g_pending_key >= 0) { int k = g_pending_key; g_pending_key = -1; return k; }
    return con_inkey(cs);
}

// A record variable has no scalar value of its own, so it is not a number and
// not a string: these four report "not found" for one rather than handing back
// the unused num/s members. Records are reached through the rec_* services.
int svc_get_num(const char *name, double *out) {
    for (int i = 0; i < var_n; i++)
        if (s_eq(vars[i].name, name) && !vars[i].is_str && !vars[i].is_rec) {
            *out = vars[i].num; return 1;
        }
    *out = 0;
    return 0;
}
void svc_set_num(const char *name, double val) {
    var_t *v = var_find(name);
    if (v && !v->is_str && !v->is_rec) v->num = trunc_int(v->is_int, val);
}
int svc_get_str(const char *name, char *buf, int buflen) {
    for (int i = 0; i < var_n; i++)
        if (s_eq(vars[i].name, name) && vars[i].is_str && !vars[i].is_rec) {
            int n = vars[i].s.slen, c = n < buflen ? n : buflen;
            for (int k = 0; k < c; k++) buf[k] = vars[i].s.sptr[k];
            return n;                              // full length, even if truncated
        }
    return 0;
}
void svc_set_str(const char *name, const char *buf, int len) {
    var_t *v = var_find(name);
    if (v && v->is_str && !v->is_rec) str_store(v, buf, len);
}
double *svc_num_array(const char *name, int *out_len) {
    arr_t *a = arr_find(name);
    if (!a || a->is_str) { if (out_len) *out_len = 0; return 0; }
    if (out_len) *out_len = a->total;
    return &arr_nums[a->off];                      // pool never moves: safe to hand out
}

// --- records ----------------------------------------------------------------
// The same zero-copy bargain as num_array, for TYPE records: an element's
// numeric fields are contiguous, so a record array is a strided double array.

const var_t *svc_rec_find(const char *name) {
    const var_t *v = var_lookup(name);
    return (v && v->is_rec) ? v : 0;
}

double *svc_rec_array(const char *name, int *nelem, int *stride) {
    const var_t *v = svc_rec_find(name);
    if (!v) { if (nelem) *nelem = 0; if (stride) *stride = 0; return 0; }
    if (nelem)  *nelem  = v->nelem;
    if (stride) *stride = types[v->rtype].n_num;
    return &rec_nums[v->roff_num];                 // pool never moves
}

int svc_rec_field(const char *name, const char *field) {
    const var_t *v = svc_rec_find(name);
    if (!v) return -1;
    const type_t *ty = &types[v->rtype];
    int f = type_field_in(ty, field);
    if (f < 0 || ty->fis_str[f]) return -1;        // text fields aren't in the block
    return ty->fslot[f];
}

// Resolve a text field to its descriptor; 0 if the record/element/field is not
// one. Shared by the two string services below.
strdesc_t *svc_rec_str_slot(const char *name, int elem, const char *field) {
    const var_t *v = svc_rec_find(name);
    if (!v || elem < 0 || elem >= v->nelem) return 0;
    const type_t *ty = &types[v->rtype];
    int f = type_field_in(ty, field);
    if (f < 0 || !ty->fis_str[f]) return 0;
    return &rec_strs[rec_str_slot(v, elem, f)];
}

int svc_rec_get_str(const char *name, int elem, const char *field,
                           char *buf, int buflen) {
    const strdesc_t *d = svc_rec_str_slot(name, elem, field);
    if (!d) return 0;
    int n = d->slen, c = n < buflen ? n : buflen;
    for (int k = 0; k < c; k++) buf[k] = d->sptr[k];
    return n;                                      // full length, even if truncated
}

void svc_rec_set_str(const char *name, int elem, const char *field,
                            const char *buf, int len) {
    strdesc_t *d = svc_rec_str_slot(name, elem, field);
    if (d) str_store_to(d, buf, len);
}
void svc_set_return_str(const char *buf, int len) {
    if (len > MAX_STR) len = MAX_STR;
    for (int i = 0; i < len; i++) g_seed_retstr[i] = buf[i];
    g_seed_retstr_len = len;
}
uint32_t svc_time_cs(void) { return (uint32_t)(con_micros() / 10000ULL); }
void *svc_alloc(unsigned nbytes) { return seed_alloc(nbytes); }
void  svc_free(void *ptr)        { seed_free(ptr); }
void *svc_realloc(void *ptr, unsigned nbytes) { return seed_realloc(ptr, nbytes); }
void *svc_alloc_aligned(unsigned a, unsigned n) { return seed_alloc_aligned(a, n); }

// GPIO passthroughs (see gpio.h). The driver validates the pin range itself, so
// these are thin; on the host build every gpio_* is a stub and gpio_available()
// is 0, which a seed can test via svc->gpio_avail().
int  svc_gpio_avail(void)                       { return gpio_available(); }
int  svc_gpio_mode(int pin, int mode, int alt)  { return gpio_set_mode(pin, mode, alt); }
int  svc_gpio_pull(int pin, int pull)           { return gpio_set_pull(pin, pull); }
void svc_gpio_write(int pin, int level)         { gpio_write(pin, level); }
int  svc_gpio_read(int pin)                     { return gpio_read(pin); }
void svc_gpio_set(uint32_t mask)                { gpio_set_mask(mask); }
void svc_gpio_clr(uint32_t mask)                { gpio_clr_mask(mask); }
uint32_t svc_gpio_level(void)                   { return gpio_read_all(); }
int  svc_gpio_wait(int pin, int edge, int cs)   { return gpio_wait_edge(pin, edge, cs); }

// SD-card files: thin adapters over the storage channel API (see storage.h),
// which the seed <stdio.h> is built on. All of this shares the file channels and
// filesystem (long names included) with BASIC's OPENIN/OPENOUT.
int svc_file_open(const char *name, int mode) {
    int m = (mode == SEED_FOPEN_WRITE)  ? STG_M_WRITE
          : (mode == SEED_FOPEN_UPDATE) ? STG_M_UPDATE : STG_M_READ;
    return stg_open(name, m);                         // channel > 0, or 0 on failure
}
int svc_file_close(int fh) { return stg_close(fh); }
int svc_file_read(int fh, void *buf, int n) {
    int r = stg_readn(fh, buf, n);                    // bulk (sector-run) read
    return r < 0 ? 0 : r;                             // short count at EOF (stdio semantics)
}
int svc_file_write(int fh, const void *buf, int n) {
    const unsigned char *p = buf;
    for (int i = 0; i < n; i++) { int r = stg_putb(fh, p[i]); if (r < 0) return i > 0 ? i : r; }
    return n;
}
long svc_file_seek(int fh, long off, int whence) {
    long base = (whence == 1) ? stg_tell(fh) : (whence == 2) ? stg_size(fh) : 0;
    if (base < 0) return base;
    long pos = base + off;
    int r = stg_seek(fh, pos);
    return r < 0 ? (long)r : pos;
}
long svc_file_size(int fh)   { return stg_size(fh); }
int  svc_file_eof(int fh)    { return stg_eof(fh); }
int  svc_file_remove(const char *name) { return stg_delete(name); }

// Format a double exactly as PRINT/STR$ do, so a seed's printf %f matches BASIC.
int svc_fmt_num(double v, char *out) { return dbl_to_str(out, v); }

// --- graphics + TrueType text for seeds (ABI v7) ---------------------------
// Device-pixel drawing forwards to gfx.h (framebuffer target / host no-op);
// font management reuses the same ttf.h engine as BASIC's GTEXT.
int  svc_gfx_avail(void)  { return sgfx_avail(); }
int  svc_gfx_width(void)  { return sgfx_width(); }
int  svc_gfx_height(void) { return sgfx_height(); }
void svc_gfx_clear(uint32_t rgb) { sgfx_clear(rgb); }
void svc_gfx_putpixel(int x, int y, uint32_t rgb) { sgfx_putpixel(x, y, rgb); }
uint32_t svc_gfx_getpixel(int x, int y) { return sgfx_getpixel(x, y); }
void svc_gfx_line(int x1, int y1, int x2, int y2, uint32_t rgb) { sgfx_line(x1, y1, x2, y2, rgb); }
void svc_gfx_fillrect(int x1, int y1, int x2, int y2, uint32_t rgb) { sgfx_fillrect(x1, y1, x2, y2, rgb); }
void svc_gfx_blit8(const unsigned char *idx, const unsigned int *pal, int w, int h, int dx, int dy, int scale) { sgfx_blit8(idx, pal, w, h, dx, dy, scale); }
void svc_gfx_circle(int cx, int cy, int r, uint32_t rgb) { sgfx_circle(cx, cy, r, rgb); }
void svc_gfx_fillcircle(int cx, int cy, int r, uint32_t rgb) { sgfx_fillcircle(cx, cy, r, rgb); }
void svc_gfx_ellipse(int cx, int cy, int rx, int ry, uint32_t rgb) { sgfx_ellipse(cx, cy, rx, ry, rgb); }
void svc_gfx_fillellipse(int cx, int cy, int rx, int ry, uint32_t rgb) { sgfx_fillellipse(cx, cy, rx, ry, rgb); }
void svc_gfx_fillpoly(const int *xy, int npts, uint32_t rgb) { sgfx_fillpoly(xy, npts, rgb); }
void svc_gfx_line_style(int w, int j, int c) { sgfx_line_style(w, j, c); }
void svc_gfx_polyline(const int *pts, int n, int cl, uint32_t rgb) { sgfx_polyline(pts, n, cl, rgb); }
void svc_gfx_flood(int x, int y, uint32_t rgb) { sgfx_flood(x, y, rgb); }
void svc_gfx_clip(int x1, int y1, int x2, int y2) { sgfx_clip(x1, y1, x2, y2); }
void svc_gfx_noclip(void) { sgfx_noclip(); }
int  svc_font_load(const char *name) { return ttf_load(name); }
int  svc_font_select(int handle) { return ttf_select(handle); }
void svc_font_size(int px) { ttf_set_size(px); }
void svc_font_style(int b, int i, int u) { ttf_set_style(b, i, u); }
void svc_gfx_text(int x, int y, const char *s, int len, uint32_t rgb) { sgfx_text(x, y, s, len, rgb); }
int  svc_text_width(const char *s, int len) { return ttf_text_width(s, len); }
int  svc_text_height(void) { return ttf_line_height(); }

// spawn/mkdir round out the unified ABI. A seed cannot launch another POD (that
// path needs the loader context the /sys shell has), so spawn is refused with
// -1; mkdir maps straight to storage, which the interpreter fully supports.
int  svc_spawn(const char *path, int argc, const char *const *argv)
                                        { (void)path; (void)argc; (void)argv; return -1; }
int  svc_mkdir(const char *path) { return stg_mkdir(path); }

/* run_basic: run a BASIC program synchronously, on the visible screen, returning
 * when it stops - so a tool like the editor can repaint itself afterwards (its
 * POD state stays live in memory the whole time). This mirrors a BASIC
 * `RUN "file"`: the caller is a POD dispatched by pod_run_program, which reads no
 * more tokens once it returns, so re-entering the run loop here is safe. g_err is
 * saved/restored so the program's own error (already reported by the run) does
 * not leak into the caller's immediate-mode context. */
int  load_bas_file(const char *name);                /* interp_files.inc   */
void run_program_once(int start_pc, int start_off);  /* interp_control.inc */
int  svc_run_basic(const char *path) {
    int saved_err;
    char saved_cwd[128];
    const char *cw;
    if (!path || !path[0]) return -1;
    if (load_bas_file(path) < 0) return -2;
    saved_err = g_err; g_err = 0;
    cw = stg_cwd();                          // remember the directory to return to
    s_copy(saved_cwd, cw ? cw : "/", sizeof saved_cwd);
    con_backbuffer(0);          // draw to the visible screen, not an unshown back buffer
    con_cls();                  // a clean screen for the program's output
    run_program(0, 0);          // run as a top-level program AND honour CHAIN - the
                                // program may launch others (e.g. the example browser
                                // menu.bas CHAINs the demo the user picks); reports errors
    stg_chdir(saved_cwd);       // a program may CD; put the caller (the editor) back
    g_err = saved_err;
    return 0;
}

// Designated initialisers: each entry names its field, so the table stays
// correct no matter how BerryServices grows or is reordered (a positional list
// silently mis-assigns on any struct change - see the history of this file).
// A field left out here is simply 0/NULL.
const BerryServices g_svc = {
    .abi_version = BERRY_ABI_VERSION,
    .putc = svc_putc, .puts = svc_puts, .getkey = svc_getkey, .inkey = svc_inkey,
    .get_num = svc_get_num, .set_num = svc_set_num,
    .get_str = svc_get_str, .set_str = svc_set_str,
    .num_array = svc_num_array, .set_return_str = svc_set_return_str, .time_cs = svc_time_cs,
    .alloc = svc_alloc, .free = svc_free,
    .realloc = svc_realloc, .alloc_aligned = svc_alloc_aligned,
    .gpio_avail = svc_gpio_avail, .gpio_mode = svc_gpio_mode, .gpio_pull = svc_gpio_pull,
    .gpio_write = svc_gpio_write, .gpio_read = svc_gpio_read,
    .gpio_set = svc_gpio_set, .gpio_clr = svc_gpio_clr,
    .gpio_level = svc_gpio_level, .gpio_wait = svc_gpio_wait,
    .file_open = svc_file_open, .file_close = svc_file_close,
    .file_read = svc_file_read, .file_write = svc_file_write,
    .file_seek = svc_file_seek, .file_size = svc_file_size,
    .file_eof = svc_file_eof, .file_remove = svc_file_remove,
    .fmt_num = svc_fmt_num,
    .gfx_avail = svc_gfx_avail, .gfx_width = svc_gfx_width,
    .gfx_height = svc_gfx_height, .gfx_clear = svc_gfx_clear,
    .gfx_putpixel = svc_gfx_putpixel, .gfx_getpixel = svc_gfx_getpixel,
    .gfx_line = svc_gfx_line, .gfx_fillrect = svc_gfx_fillrect,
    .gfx_circle = svc_gfx_circle, .gfx_fillcircle = svc_gfx_fillcircle,
    .gfx_ellipse = svc_gfx_ellipse, .gfx_fillellipse = svc_gfx_fillellipse,
    .gfx_fillpoly = svc_gfx_fillpoly, .gfx_flood = svc_gfx_flood,
    .gfx_clip = svc_gfx_clip, .gfx_noclip = svc_gfx_noclip,
    .font_load = svc_font_load, .font_select = svc_font_select,
    .font_size = svc_font_size, .font_style = svc_font_style,
    .gfx_text = svc_gfx_text, .text_width = svc_text_width, .text_height = svc_text_height,
    .rec_array = svc_rec_array, .rec_field = svc_rec_field,
    .rec_get_str = svc_rec_get_str, .rec_set_str = svc_rec_set_str,
    .mouse = svc_mouse,
    .gfx_backbuffer = svc_gfx_backbuffer, .gfx_flip = svc_gfx_flip,
    .gfx_buffered = svc_gfx_buffered,
    .keymods = svc_keymods,
    .gfx_mode = svc_gfx_mode,
    .gfx_line_style = svc_gfx_line_style, .gfx_polyline = svc_gfx_polyline,
    .spawn = svc_spawn, .mkdir = svc_mkdir,
    .vdu = svc_vdu, .screen_size = svc_screen_size,
    .con_font = svc_con_font, .con_glyph = svc_con_glyph,
    .dir_open = svc_dir_open, .dir_read = svc_dir_read,
    .getcwd = svc_getcwd, .chdir = svc_chdir,
    .clip_set = svc_clip_set, .clip_get = svc_clip_get, .clip_len = svc_clip_len,
    .icache_sync = svc_icache_sync,
    .gfx_blit8 = svc_gfx_blit8,
    .keys_down = svc_keys_down,
    .audio_open = svc_audio_open, .audio_avail = svc_audio_avail,
    .audio_write = svc_audio_write, .audio_close = svc_audio_close,
    .run_basic = svc_run_basic,
};

// Parse "handle [, arg ...]" (tok at the handle, stops on the first non-comma
// token), invoke the seed, and return its numeric result. String arguments are
// snapshotted into scratch so they stay valid even if the seed triggers GC by
// writing a variable. g_seed_retstr[_len] receives any string result.
double seed_run_collect(void) {
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

// A dynamic (runtime-registered) keyword. Both native seeds (from /seed at
// startup) and POD extensions (PODLOAD) live in this one table, so the lexer and
// dispatch treat them alike. is_pod picks the backend: a native seed runs from
// its persistent code block via `entry`; a POD keyword runs from pod_entry
// (inside a resident POD image) through its own capability-gated pod_svc.

seed_kw_t  seed_kw_tab[SEED_KW_MAX];
int        seed_kw_n = 0;

// Find a registered keyword by (already upper-cased) name; -1 if none.
int seed_kw_lookup(const char *name) {
    for (int i = 0; i < seed_kw_n; i++)
        if (s_eq(name, seed_kw_tab[i].name)) return i;
    return -1;
}

// The spelling of a seed's keyword, by registry index. The counterpart of
// kw_spelling() for built-ins: a type or field may be named after a keyword,
// and a seed-registered one is no different (a seed adding PARTICLE must not
// stop you writing TYPE particle).
const char *seed_kw_name(int idx) {
    if (idx < 0 || idx >= seed_kw_n) return 0;
    return seed_kw_tab[idx].name;
}

// Bytes to reserve for a loaded seed: the header's image_size (total footprint
// including .bss, stamped by tcc -seed) when it is known, else the blob length
// plus a 16 KB zeroed margin for a gcc-built seed's .bss (matching the old fixed
// slot, which zeroed 16 KB). Rounded up to a page.
unsigned seed_footprint(const struct seed_header *hdr, int len) {
    unsigned foot = hdr->image_size ? hdr->image_size : (unsigned)len + 16384u;
    if (foot < (unsigned)len) foot = (unsigned)len;
    return (foot + 4095u) & ~4095u;
}

// Validate a loaded .sed blob and, if it registers a keyword, copy it into a
// persistent code block and add it to the table. Returns 1 if a keyword was
// installed.
int seed_kw_register(const char *blob, int len) {
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
int seed_fill_arg(berry_arg *a, value_t v) {
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
int seed_gather_args(berry_arg *argv, int max, int paren) {
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
int seed_kw_invoke(seed_kw_t *k, berry_arg *argv, int argc, double *out) {
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
value_t eval_seed_keyword(int id) {
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
void exec_seed_keyword(int id) {
    seed_kw_t *k = &seed_kw_tab[id - KW_SEED_DYN];
    lex_next();                                            // consume the keyword
    berry_arg argv[SEED_MAX_ARGS];
    int argc = seed_gather_args(argv, SEED_MAX_ARGS, 0);
    if (argc < 0) return;
    double ret;
    seed_kw_invoke(k, argv, argc, &ret);
}
