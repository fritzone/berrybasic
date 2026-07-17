// ===========================================================================
// BerryBasiC — data model: program store, values, variables, arrays, string heap
//
// This file is a fragment of the interpreter and is #included by basic.c.
// It is NOT a standalone translation unit: it shares the single translation
// unit's static state and is compiled only as part of basic.c. Do not add it
// to the build system or compile it on its own.
// ===========================================================================
// ---------------------------------------------------------------------------
// Program storage: sorted-by-line-number table of source lines
// ---------------------------------------------------------------------------

// A program line. `module` is 0 for the main program and 1+ for lines pulled in
// by IMPORT; line-number lookups (GOTO/GOSUB/labels/DATA) are scoped to the
// running line's module, so a module's line numbers may freely overlap the main
// program's. Module lines are appended at RUN and stripped when it ends.
typedef struct { int num; int module; char text[LINE_LEN]; } progline_t;

static progline_t prog[MAX_LINES];
static int        prog_n = 0;            // total lines (main + any imported modules)
static int        main_n = 0;            // main-program lines only = prog[0..main_n)

static int cur_module(void);             // module of the currently executing line

static int line_is_blank(const char *t) {
    for (; *t; t++) if (!is_space(*t)) return 0;
    return 1;
}

static void prog_store(int num, const char *text) {
    int i = 0;
    while (i < prog_n && prog[i].num < num) i++;

    if (i < prog_n && prog[i].num == num) {       // replace or delete existing
        if (line_is_blank(text)) {                // "10" alone deletes line 10
            for (int j = i; j < prog_n - 1; j++) prog[j] = prog[j + 1];
            prog_n--;
        } else {
            s_copy(prog[i].text, text, LINE_LEN);
        }
        return;
    }
    if (line_is_blank(text)) return;              // delete of nonexistent line
    if (prog_n >= MAX_LINES) { err("Out of memory"); return; }

    for (int j = prog_n; j > i; j--) prog[j] = prog[j - 1];  // insert, keep sorted
    prog[i].num = num;
    prog[i].module = 0;
    s_copy(prog[i].text, text, LINE_LEN);
    prog_n++;
}

// Resolve a line number within the current module only, so overlapping numbers
// in the main program and an imported module never clash.
static int find_line_index(int num) {
    int m = cur_module();
    for (int i = 0; i < prog_n; i++)
        if (prog[i].module == m && prog[i].num == num) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Runtime values: a value is either a number (long) or a string (ptr+len).
// String bytes are NOT NUL-terminated; the length is authoritative.
// ---------------------------------------------------------------------------

typedef struct {
    int    is_str;
    double num;
    char  *str;
    int    len;
} value_t;

static value_t v_num(double n)          { value_t v; v.is_str = 0; v.num = n; v.str = 0; v.len = 0; return v; }
static value_t v_str(char *p, int len)  { value_t v; v.is_str = 1; v.num = 0; v.str = p; v.len = len; return v; }

// A string value living in the GC heap: bytes pointer + length.
typedef struct { char *sptr; int slen; } strdesc_t;

static int name_is_str(const char *name) {
    int i = 0; while (name[i]) i++;
    return i > 0 && name[i - 1] == '$';
}
static int name_is_int(const char *name) {           // A%, COUNT% etc. are integers
    int i = 0; while (name[i]) i++;
    return i > 0 && name[i - 1] == '%';
}
static double trunc_int(int is_int, double x) {      // BBC % vars truncate toward zero
    return is_int ? (double)(long)x : x;
}

// ---------------------------------------------------------------------------
// Scalar variables. A variable holds either a number or a string descriptor;
// the trailing '$' in the name selects which (A and A$ are distinct).
// ---------------------------------------------------------------------------

typedef struct {
    char      name[NAME_LEN];
    int       is_str;
    int       is_int;        // % suffix: value is kept truncated to an integer
    double    num;
    strdesc_t s;             // string bytes live in the GC heap (gcheap)
    // Record variables (DIM p AS point). When is_rec is set, num/s are unused
    // and the fields below say where this record's storage lives; see the
    // user-defined type section for what the offsets index into.
    int       is_rec;
    int       rtype;         // index into types[]
    int       nelem;         // 1 = scalar record; >1 = array of records
    int       roff_num;      // base index into rec_nums[]
    int       roff_str;      // base index into rec_strs[]
} var_t;

static var_t vars[MAX_VARS];
static int   var_n = 0;

static var_t *var_lookup(const char *name) {     // find, without creating
    for (int i = 0; i < var_n; i++)
        if (s_eq(vars[i].name, name)) return &vars[i];
    return 0;
}

static var_t *var_find(const char *name) {
    var_t *e = var_lookup(name);
    if (e) return e;
    if (var_n >= MAX_VARS) { err("Out of memory"); return 0; }
    var_t *v = &vars[var_n++];
    s_copy(v->name, name, NAME_LEN);
    v->is_str = name_is_str(name);
    v->is_int = name_is_int(name);
    v->num = 0; v->s.sptr = 0; v->s.slen = 0;
    v->is_rec = 0; v->rtype = 0; v->nelem = 0;
    v->roff_num = 0; v->roff_str = 0;
    return v;
}

// ---------------------------------------------------------------------------
// Arrays. Element storage is bump-allocated from fixed pools (no malloc):
// numeric elements from arr_nums[], string elements from arr_strs[] (each a
// strdesc into the GC heap). String array elements are GC roots too.
// ---------------------------------------------------------------------------

#define MAX_ARRAYS      16
#define MAX_DIMS         3
#define ARR_NUM_POOL  4096      // numeric elements (longs)
#define ARR_STR_POOL   512      // string elements (descriptors)

typedef struct {
    char name[NAME_LEN];
    int  is_str;
    int  is_int;               // % suffix: integer elements
    int  ndim;
    int  dim[MAX_DIMS];        // element count per dimension (DIM value + 1)
    int  total;                // product of dims
    int  off;                  // base index into arr_nums[] or arr_strs[]
} arr_t;

static arr_t     arrs[MAX_ARRAYS];
static int       arr_n = 0;
static double    arr_nums[ARR_NUM_POOL];
static int       arr_nums_top = 0;
static strdesc_t arr_strs[ARR_STR_POOL];
static int       arr_strs_top = 0;

static arr_t *arr_find(const char *name) {
    for (int i = 0; i < arr_n; i++)
        if (s_eq(arrs[i].name, name)) return &arrs[i];
    return 0;
}

static arr_t *arr_create(const char *name, int ndim, const int *counts, int is_str) {
    if (arr_n >= MAX_ARRAYS) { err("Out of memory"); return 0; }
    if (ndim < 1 || ndim > MAX_DIMS) { err("Array index out of range"); return 0; }
    int total = 1;
    for (int i = 0; i < ndim; i++) total *= counts[i];
    arr_t *a = &arrs[arr_n];
    s_copy(a->name, name, NAME_LEN);
    a->is_str = is_str;
    a->is_int = name_is_int(name);
    a->ndim = ndim;
    a->total = total;
    for (int i = 0; i < ndim; i++) a->dim[i] = counts[i];
    if (is_str) {
        if (arr_strs_top + total > ARR_STR_POOL) { err("Out of memory"); return 0; }
        a->off = arr_strs_top;
        for (int k = 0; k < total; k++) { arr_strs[a->off + k].sptr = 0; arr_strs[a->off + k].slen = 0; }
        arr_strs_top += total;
    } else {
        if (arr_nums_top + total > ARR_NUM_POOL) { err("Out of memory"); return 0; }
        a->off = arr_nums_top;
        for (int k = 0; k < total; k++) arr_nums[a->off + k] = 0;
        arr_nums_top += total;
    }
    arr_n++;
    return a;
}

// ---------------------------------------------------------------------------
// User-defined types (records).
//
//   TYPE point : x, y$ : ENDTYPE      defines the shape
//   DIM p AS point                    gives one instance storage
//   DIM e(10) AS point                gives eleven
//
// A type records its field names and, for each, a *slot* index: fields are
// stored split by kind, so an element's numeric fields occupy n_num slots in
// rec_nums[] and its string fields n_str slots in rec_strs[]. fslot[f] is the
// field's index within whichever of the two it belongs to. An instance is then
// just a base offset into each pool, and element e of field f lives at
// base + e*n_num + fslot[f] (or the string equivalent).
//
// A record *variable* is an ordinary var_t with is_rec set. That is the whole
// trick: because records live in vars[], PROC/FN parameter binding and LOCAL
// save/restore already handle them, and binding a parameter copies only the
// offsets - which is exactly what makes record arguments by-reference.
// ---------------------------------------------------------------------------

#define MAX_TYPES       8
#define MAX_FIELDS     16
#define REC_NUM_POOL 1024      // numeric field slots across all record instances
#define REC_STR_POOL  256      // string field slots (descriptors into gcheap)

typedef struct {
    char name[NAME_LEN];
    int  nf;                        // field count
    char fname[MAX_FIELDS][NAME_LEN];
    int  fis_str[MAX_FIELDS];
    int  fis_int[MAX_FIELDS];
    int  fslot[MAX_FIELDS];         // index among this element's numeric or string slots
    int  n_num, n_str;              // slots per element
} type_t;

static type_t types[MAX_TYPES];
static int    type_n = 0;

static double    rec_nums[REC_NUM_POOL];
static int       rec_nums_top = 0;
static strdesc_t rec_strs[REC_STR_POOL];
static int       rec_strs_top = 0;

static int type_find(const char *name) {
    for (int i = 0; i < type_n; i++)
        if (s_eq(types[i].name, name)) return i;
    return -1;
}

static int type_field_in(const type_t *ty, const char *name) {
    for (int i = 0; i < ty->nf; i++)
        if (s_eq(ty->fname[i], name)) return i;
    return -1;
}

// Reserve field storage for `nelem` elements of type `t`, zeroed. Returns 0 on
// error (having raised it).
static int rec_alloc(int t, int nelem, int *off_num, int *off_str) {
    const type_t *ty = &types[t];
    int nn = ty->n_num * nelem, ns = ty->n_str * nelem;
    if (rec_nums_top + nn > REC_NUM_POOL) { err("Out of memory"); return 0; }
    if (rec_strs_top + ns > REC_STR_POOL) { err("Out of memory"); return 0; }
    *off_num = rec_nums_top;
    *off_str = rec_strs_top;
    for (int i = 0; i < nn; i++) rec_nums[rec_nums_top + i] = 0;
    for (int i = 0; i < ns; i++) { rec_strs[rec_strs_top + i].sptr = 0; rec_strs[rec_strs_top + i].slen = 0; }
    rec_nums_top += nn;
    rec_strs_top += ns;
    return 1;
}

// Pool index of field f of element e of record variable v.
static int rec_num_slot(const var_t *v, int e, int f) {
    const type_t *ty = &types[v->rtype];
    return v->roff_num + e * ty->n_num + ty->fslot[f];
}
static int rec_str_slot(const var_t *v, int e, int f) {
    const type_t *ty = &types[v->rtype];
    return v->roff_str + e * ty->n_str + ty->fslot[f];
}

// ---------------------------------------------------------------------------
// String storage.
//   * gcheap[]  holds the bytes of string *variables and array elements*;
//     compacted by gc().
//   * scratch[] holds short-lived temporaries (literals, concatenation and
//     function results) produced while evaluating one statement; it is rewound
//     to empty at the start of every statement, so its contents never need GC.
//
// gc() is only ever reached from a string store, where the source bytes have
// already been staged into a private buffer. No temporary in scratch is ever a
// GC root, and no live value points into gcheap across a store, so compaction
// can freely relocate strings.
// ---------------------------------------------------------------------------

static char gcheap[GCHEAP_SIZE];
static int  gcheap_top = 0;

static char scratch[SCRATCH_SIZE];
static int  scratch_top = 0;
static int  scratch_base = 0;   // PROC/FN bodies rewind here, preserving caller temporaries

static void scratch_reset(void) { scratch_top = scratch_base; }

static char *scratch_alloc(int n) {
    if (scratch_top + n > SCRATCH_SIZE) { err("Expression too complex"); return 0; }
    char *p = &scratch[scratch_top];
    scratch_top += n;
    return p;
}

// Saved variable values for PROC/FN parameters and LOCAL declarations. These
// hold string descriptors into the GC heap, so they must be GC roots too.
#define LOCAL_MAX 96
typedef struct { var_t *slot; var_t old; } localsave_t;
static localsave_t local_stack[LOCAL_MAX];
static int         local_sp = 0;

// Mark-compact the GC heap: slide every live string down to remove the gaps
// left by overwritten/temporary strings, fixing up the descriptors. Roots are
// scalar string variables, string-array elements, record string fields, and
// saved locals.
static strdesc_t *gc_roots[MAX_VARS + ARR_STR_POOL + REC_STR_POOL + LOCAL_MAX];

static void gc(void) {
    int n = 0;
    for (int i = 0; i < var_n; i++)
        if (vars[i].is_str && vars[i].s.slen > 0) gc_roots[n++] = &vars[i].s;
    for (int i = 0; i < arr_n; i++)
        if (arrs[i].is_str)
            for (int k = 0; k < arrs[i].total; k++) {
                strdesc_t *d = &arr_strs[arrs[i].off + k];
                if (d->slen > 0) gc_roots[n++] = d;
            }
    // Every allocated record string slot, rooted straight from the pool: a slot
    // may be shared by several var_t (a by-reference parameter aliases its
    // caller's record), so rooting via variables would double-count it.
    for (int i = 0; i < rec_strs_top; i++)
        if (rec_strs[i].slen > 0) gc_roots[n++] = &rec_strs[i];
    for (int i = 0; i < local_sp; i++)
        if (local_stack[i].old.is_str && local_stack[i].old.s.slen > 0)
            gc_roots[n++] = &local_stack[i].old.s;
    for (int i = 1; i < n; i++) {                // insertion sort by sptr
        strdesc_t *k = gc_roots[i];
        int j = i - 1;
        while (j >= 0 && gc_roots[j]->sptr > k->sptr) { gc_roots[j + 1] = gc_roots[j]; j--; }
        gc_roots[j + 1] = k;
    }
    int top = 0;
    for (int i = 0; i < n; i++) {
        char *src = gc_roots[i]->sptr;
        int   len = gc_roots[i]->slen;
        char *dst = &gcheap[top];
        if (dst != src) for (int k = 0; k < len; k++) dst[k] = src[k];  // dst <= src
        gc_roots[i]->sptr = dst;
        top += len;
    }
    gcheap_top = top;
}

static char *gc_alloc(int n) {
    if (gcheap_top + n > GCHEAP_SIZE) gc();
    if (gcheap_top + n > GCHEAP_SIZE) { err("Out of memory"); return 0; }
    char *p = &gcheap[gcheap_top];
    gcheap_top += n;
    return p;
}

// Store len bytes from src into the string descriptor d. src may point anywhere
// (scratch, program text, or another string in gcheap), so it is staged into a
// private buffer before gc_alloc(), which may relocate gcheap.
static void str_store_to(strdesc_t *d, const char *src, int len) {
    static char stage[MAX_STR];
    if (len > MAX_STR) { err("Text string is too long"); return; }
    for (int i = 0; i < len; i++) stage[i] = src[i];
    char *dst = gc_alloc(len);
    if (!dst) return;
    for (int i = 0; i < len; i++) dst[i] = stage[i];
    d->sptr = dst; d->slen = len;
}

static void str_store(var_t *v, const char *src, int len) {
    str_store_to(&v->s, src, len);
    if (!g_err) v->is_str = 1;
}

// Memory reserved by `DIM name size`: a byte arena that BASIC hands out real
// addresses into, for use with the ?/!/$ indirection operators and for passing
// buffers to native seeds. Reset (like the variable/array pools) on RUN/NEW.
#define DIM_HEAP_SIZE (256 * 1024)   // room for indirection buffers and sprites (GGET/GPUT)
// 16-aligned so a sprite's pixel area (base+8) is 4-aligned: SPRITETARGET renders
// into it with the graphics driver's aligned 32-bit stores (-mstrict-align).
static unsigned char dim_heap[DIM_HEAP_SIZE] __attribute__((aligned(16)));
static int           dim_top = 0;

static void clear_vars(void) {       // BASIC CLR: scalars + arrays + records + string heap
    var_n = 0;
    gcheap_top = 0;
    arr_n = 0;
    arr_nums_top = 0;
    arr_strs_top = 0;
    type_n = 0;                  // TYPE definitions are declarations, like DIM
    rec_nums_top = 0;
    rec_strs_top = 0;
    dim_top = 0;
    img_sprite_reset();      // free image-loaded sprites
    ttf_reset();             // drop loaded TrueType fonts
    gpio_reset();            // header pins -> input, no pull (never leave a load driven)
    i2c_reset();             // release the I2C bus (gpio_reset returned SDA/SCL to inputs)
}

// Reserve `nbytes` from the DIM arena, 8-byte aligned; returns the base address
// (as an integer that fits exactly in a double), or 0 if the arena is full.
static long dim_reserve(int nbytes) {
    dim_top = (dim_top + 7) & ~7;
    if (nbytes < 0 || dim_top + nbytes > DIM_HEAP_SIZE) { err("Out of memory"); return 0; }
    long base = (long)(uintptr_t)&dim_heap[dim_top];
    dim_top += nbytes;
    return base;
}

// Alignment-safe peek/poke (byte-wise, so -mstrict-align never faults). Words
// are little-endian 32-bit; reads sign-extend. `$` strings are CR-terminated.
static long mem_peekb(long a) { return *(volatile unsigned char *)(uintptr_t)a; }
static void mem_pokeb(long a, long v) { *(volatile unsigned char *)(uintptr_t)a = (unsigned char)v; }
static long mem_peekw(long a) {
    const volatile unsigned char *p = (const volatile unsigned char *)(uintptr_t)a;
    unsigned w = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
    return (long)(int)w;
}
static void mem_pokew(long a, long v) {
    volatile unsigned char *p = (volatile unsigned char *)(uintptr_t)a;
    unsigned w = (unsigned)v;
    p[0] = (unsigned char)w; p[1] = (unsigned char)(w >> 8);
    p[2] = (unsigned char)(w >> 16); p[3] = (unsigned char)(w >> 24);
}

