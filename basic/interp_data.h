#ifndef INTERP_DATA_H
#define INTERP_DATA_H

#include "interp_types.h"

/* ==================================================================
 * interp_data.c -- The program store, typed values, scalar/array/record variables, the
 * user-defined TYPE machinery, and the GC'd string heap.
 * ================================================================== */

/* -------------------------------------------------------------- globals */

extern int       arr_n;
extern double    arr_nums[ARR_NUM_POOL];
extern int       arr_nums_top;
extern strdesc_t arr_strs[ARR_STR_POOL];
extern int       arr_strs_top;
extern arr_t     arrs[MAX_ARRAYS];
extern int           dim_top;
extern strdesc_t *gc_roots[MAX_VARS + ARR_STR_POOL + REC_STR_POOL + LOCAL_MAX];
extern char gcheap[GCHEAP_SIZE];
extern int  gcheap_top;
extern int         local_sp;
extern localsave_t local_stack[LOCAL_MAX];
extern int        main_n;   // main-program lines only = prog[0..main_n)
extern progline_t prog[MAX_LINES];
extern int        prog_n;   // total lines (main + any imported modules)
extern double    rec_nums[REC_NUM_POOL];
extern int       rec_nums_top;
extern strdesc_t rec_strs[REC_STR_POOL];
extern int       rec_strs_top;
extern char scratch[SCRATCH_SIZE];
extern int  scratch_base;   // PROC/FN bodies rewind here, preserving caller temporaries
extern int  scratch_top;
extern int    type_n;
extern type_t types[MAX_TYPES];
extern int   var_n;
extern var_t vars[MAX_VARS];

/* ------------------------------------------------------------ functions */

// Line is blank.
int line_is_blank (const char *t);

// Prog store.
void prog_store (int num, const char *text);

// Resolve a line number within the current module only, so overlapping numbers
// in the main program and an imported module never clash.
int find_line_index (int num);

// V num.
value_t v_num (double n);

// V str.
value_t v_str (char *p, int len);

// Name is str.
int name_is_str (const char *name);

// Name is int.
int name_is_int (const char *name);

// Trunc int.
double trunc_int (int is_int, double x);

// Var lookup.
var_t *var_lookup (const char *name);

// Var find.
var_t *var_find (const char *name);

// Arr find.
arr_t *arr_find (const char *name);

// Arr create.
arr_t *arr_create (const char *name, int ndim, const int *counts, int is_str);

// Type find.
int type_find (const char *name);

// Type field in.
int type_field_in (const type_t *ty, const char *name);

// Reserve field storage for `nelem` elements of type `t`, zeroed. Returns 0 on
// error (having raised it).
int rec_alloc (int t, int nelem, int *off_num, int *off_str);

// Pool index of field f of element e of record variable v.
int rec_num_slot (const var_t *v, int e, int f);

// Rec str slot.
int rec_str_slot (const var_t *v, int e, int f);

// Reset scratch to its initial state.
void scratch_reset (void);

// Scratch alloc.
char *scratch_alloc (int n);

// Gc.
void gc (void);

// Gc alloc.
char *gc_alloc (int n);

// Store len bytes from src into the string descriptor d. src may point anywhere
// (scratch, program text, or another string in gcheap), so it is staged into a
// private buffer before gc_alloc(), which may relocate gcheap.
void str_store_to (strdesc_t *d, const char *src, int len);

// Str store.
void str_store (var_t *v, const char *src, int len);

// Clear vars.
void clear_vars (void);

// Reserve `nbytes` from the DIM arena, 8-byte aligned; returns the base address
// (as an integer that fits exactly in a double), or 0 if the arena is full.
long int dim_reserve (int nbytes);

// Alignment-safe peek/poke (byte-wise, so -mstrict-align never faults). Words
// are little-endian 32-bit; reads sign-extend. `$` strings are CR-terminated.
long int mem_peekb (long int a);

// Mem pokeb.
void mem_pokeb (long int a, long int v);

// Mem peekw.
long int mem_peekw (long int a);

// Mem pokew.
void mem_pokew (long int a, long int v);

#endif /* INTERP_DATA_H */
