#ifndef BERRY_SERVICES_H
#define BERRY_SERVICES_H
// ===========================================================================
// BerryBasiC system services — the one table the interpreter hands to native
// code at entry, shared by native seeds (seed.h) and PODs (pod.h).
//
// This is the single source of truth for the services layout. It uses only
// builtin C types (no <stdint.h>), so it compiles unchanged in the freestanding
// POD environment as well as inside the kernel, and both the loader-side and
// compiler-side headers include this same file instead of each carrying a copy.
//
// APPEND-ONLY across ABI versions: new members go on the end and
// BERRY_ABI_VERSION is bumped, so seeds/PODs built against an older version keep
// working (their field offsets are unchanged). All variable names passed to the
// var/record services are uppercase with their BASIC suffix, e.g. "X", "N%", "A$".
// ===========================================================================

// One unified table replaces the old SeedServices (ABI 13) and PodServices
// (ABI 4). v14 was the first unified version; v15 appended `vdu`; v16 appended
// `screen_size`; v17 appended `con_font`/`con_glyph`; v18 appended the directory
// browsing set (dir_open/dir_read/getcwd/chdir); v19 appended the clipboard
// (clip_set/clip_get/clip_len); v20 appended icache_sync.
#define BERRY_ABI_VERSION 23u

// Modes for the file_open service.
#define BERRY_FOPEN_READ   0
#define BERRY_FOPEN_WRITE  1
#define BERRY_FOPEN_UPDATE 2

// One keyword-handler argument: a BASIC number, or a snapshot of a BASIC string
// (bytes live in interpreter scratch and are valid only for the call; copy to
// keep). Layout is shared by the seed and POD argument-gathering paths.
typedef struct berry_arg {
    int          is_str;   // 0 = number (use .num), 1 = string (use .str/.len)
    double       num;
    const char  *str;      // not NUL-terminated; .len is authoritative
    int          len;
} berry_arg;

// The services table. Grouped by facility; `abi_version` is always first. The
// POD loader fills only the groups a POD's capabilities grant and leaves every
// other slot a refusal stub; the seed path fills them all (seeds are ungated).
typedef struct BerryServices {
    unsigned abi_version;

    // --- console -----------------------------------------------------------
    void (*putc)(int c);                       // write one character
    void (*puts)(const char *s, int len);      // write len bytes (no NUL needed)
    int  (*getkey)(void);                      // block for a key, return its code
    int  (*inkey)(int centiseconds);           // wait up to n cs; -1 on timeout

    // --- BASIC scalar variables -------------------------------------------
    int  (*get_num)(const char *name, double *out);   // 1 if defined, else 0
    void (*set_num)(const char *name, double val);
    int  (*get_str)(const char *name, char *buf, int buflen);  // -> full length
    void (*set_str)(const char *name, const char *buf, int len);

    // --- numeric arrays: direct, zero-copy pointer (storage never moves) ---
    double *(*num_array)(const char *name, int *out_len);      // 0 if not found

    // --- returning a string result (read by CALL$ / a string keyword) ------
    void (*set_return_str)(const char *buf, int len);

    // --- time --------------------------------------------------------------
    unsigned (*time_cs)(void);                 // centiseconds since boot

    // --- dynamic memory ----------------------------------------------------
    // A native heap independent of BASIC's storage; persists across calls within
    // one RUN and is reclaimed wholesale at the next RUN/NEW.
    void *(*alloc)(unsigned nbytes);           // 0 if the heap is exhausted
    void  (*free)(void *ptr);                  // free a prior alloc (0 = no-op)
    void *(*realloc)(void *ptr, unsigned nbytes);            // grow/shrink a block
    void *(*alloc_aligned)(unsigned alignment, unsigned nbytes);  // alignment = pow2

    // --- GPIO --------------------------------------------------------------
    // Direct access to the Pi 4's 40-pin header (BCM numbering, pins 0..27).
    // gpio_avail is 0 on the host build (everything else then does nothing).
    int  (*gpio_avail)(void);                       // 1 on the Pi/QEMU, 0 on host
    int  (*gpio_mode)(int pin, int mode, int alt);  // GPIO_IN/OUT/ALT; <0 bad pin
    int  (*gpio_pull)(int pin, int pull);           // GPIO_PULL_*; <0 bad pin
    void (*gpio_write)(int pin, int level);         // drive an output 0/1
    int  (*gpio_read)(int pin);                     // read a pin -> 0/1
    void (*gpio_set)(unsigned mask);                // set every 1-bit pin (GPSET0)
    void (*gpio_clr)(unsigned mask);                // clear every 1-bit pin (GPCLR0)
    unsigned (*gpio_level)(void);                   // all pin levels (low 28 bits)
    int  (*gpio_wait)(int pin, int edge, int timeout_cs);  // edge GPIO_*; pin or -1

    // --- SD-card files -----------------------------------------------------
    // The minimal file interface the native <stdio.h> is built on; goes to the
    // same filesystem BASIC uses (long names included). Handles are small
    // positive ints; 0 means failure.
    int  (*file_open)(const char *name, int mode);       // FOPEN_*; handle>0 or 0
    int  (*file_close)(int fh);                          // 0 ok, <0 error
    int  (*file_read)(int fh, void *buf, int n);         // -> bytes (0 = EOF), <0 err
    int  (*file_write)(int fh, const void *buf, int n);  // -> bytes written, <0 err
    long (*file_seek)(int fh, long off, int whence);     // whence 0/1/2 -> pos, <0 err
    long (*file_size)(int fh);                           // length in bytes, <0 error
    int  (*file_eof)(int fh);                            // 1 at end-of-file, else 0
    int  (*file_remove)(const char *name);              // delete a file; 0 ok, <0 err

    // --- number formatting -------------------------------------------------
    // Format a double into `out` (>= 32 bytes) the way BASIC's PRINT does; returns
    // the character count. Used by the native <stdio.h> printf %f/%e/%g so native
    // and BASIC number output match exactly.
    int  (*fmt_num)(double v, char *out);

    // --- graphics ----------------------------------------------------------
    // Physical-pixel drawing straight onto the framebuffer BASIC is targeting
    // (the visible screen, or a BUFFER/SPRITETARGET). Origin TOP-LEFT (x right,
    // y down); colours 24-bit 0xRRGGBB. gfx_avail is 0 on the host build (draws
    // are then no-ops). This is what the BGI-style <graphics.h> is built on.
    int  (*gfx_avail)(void);                          // 1 with a framebuffer, else 0
    int  (*gfx_width)(void);                          // screen width in pixels
    int  (*gfx_height)(void);                         // screen height in pixels
    void (*gfx_clear)(unsigned rgb);                  // fill the whole surface
    void (*gfx_putpixel)(int x, int y, unsigned rgb);
    unsigned (*gfx_getpixel)(int x, int y);           // 0xRRGGBB at (x,y)
    void (*gfx_line)(int x1, int y1, int x2, int y2, unsigned rgb);
    void (*gfx_fillrect)(int x1, int y1, int x2, int y2, unsigned rgb);
    void (*gfx_circle)(int cx, int cy, int r, unsigned rgb);            // outline
    void (*gfx_fillcircle)(int cx, int cy, int r, unsigned rgb);
    void (*gfx_ellipse)(int cx, int cy, int rx, int ry, unsigned rgb);  // outline
    void (*gfx_fillellipse)(int cx, int cy, int rx, int ry, unsigned rgb);
    void (*gfx_fillpoly)(const int *xy, int npts, unsigned rgb);     // x0,y0,x1,y1,...
    void (*gfx_flood)(int x, int y, unsigned rgb);    // flood the region under (x,y)
    void (*gfx_clip)(int x1, int y1, int x2, int y2); // restrict drawing to a rect
    void (*gfx_noclip)(void);                         // remove the clip rectangle

    // --- text: TrueType fonts ----------------------------------------------
    // The same engine BASIC's GTEXT uses. Metrics work even on the host; gfx_text
    // is a no-op there. Handles are 1..N; font_load returns 0 on failure.
    int  (*font_load)(const char *name);              // load a .ttf -> handle, or 0
    int  (*font_select)(int handle);                  // 1 ok, 0 unknown handle
    void (*font_size)(int pixels);                    // glyph height in pixels
    void (*font_style)(int bold, int italic, int underline);  // 0/1 flags
    void (*gfx_text)(int x, int y, const char *s, int len, unsigned rgb);  // baseline
    int  (*text_width)(const char *s, int len);       // pixel width, current font
    int  (*text_height)(void);                        // line height in pixels

    // --- TYPE records ------------------------------------------------------
    // BASIC user-defined types reached by name, like arrays. The numeric fields
    // of one element are contiguous, so a record (or an array of records) is a
    // strided array of doubles: element `e`, field `f` is base[e*stride + f].
    // Text fields are copied by name.
    double *(*rec_array)(const char *name, int *nelem, int *stride);
    int  (*rec_field)(const char *name, const char *field);   // index, or -1
    int  (*rec_get_str)(const char *name, int elem, const char *field,
                        char *buf, int buflen);
    void (*rec_set_str)(const char *name, int elem, const char *field,
                        const char *buf, int len);

    // --- pointer / mouse ---------------------------------------------------
    // Poll the USB mouse: position in raw framebuffer pixels (the space gfx_*
    // draws in) and a button bitmask (bit0 left, bit1 right, bit2 middle). Any
    // pointer may be 0; with no mouse (or on the host) everything reads 0. The
    // call is the poll: a live-mouse loop must call this itself.
    void (*mouse)(int *x, int *y, int *buttons);

    // --- double buffering --------------------------------------------------
    // Same mechanism (and state) as BASIC's BUFFER ON / FLIP. backbuffer(1)
    // routes drawing off-screen until flip() presents it; the buffer keeps its
    // contents across flips. Restore the setting as found (buffered() reports it).
    int  (*gfx_backbuffer)(int on);                   // 0 ok, <0 could not allocate
    void (*gfx_flip)(void);
    int  (*gfx_buffered)(void);

    // --- keyboard modifiers and locks --------------------------------------
    // Which modifiers/locks are held, as a KMOD_* bitmask. A snapshot refreshed
    // when the keyboard is read, so call inkey(0) first in a polling loop. 0 with
    // no USB keyboard.
    int  (*keymods)(void);

    // --- graphics mode -----------------------------------------------------
    // The BASIC graphics mode: 1 = BBC logical (1280x1024, origin bottom-left),
    // 2 = native device pixels (origin top-left, this API's convention). Drawing
    // through gfx_* is ALWAYS device pixels regardless of the mode.
    int (*gfx_mode)(void);

    // --- line style --------------------------------------------------------
    // The pen for thick lines/outlines, in device pixels. width 1 = hairline;
    // join 0 miter/1 bevel/2 round; cap 0 butt/1 round/2 square. gfx_line/circle/
    // ellipse honour width+cap; gfx_polyline strokes a run honouring width, join
    // and cap (closed != 0 = closed polygon, joins at every vertex, no caps).
    void (*gfx_line_style)(int width, int join, int cap);
    void (*gfx_polyline)(const int *pts, int n, int closed, unsigned rgb);

    // --- spawning and directories ------------------------------------------
    // spawn loads and runs another program POD, blocking until it returns (its
    // exit status, or <0 if it could not be loaded/run); this is how a build tool
    // drives the compiler. mkdir creates a directory (0 ok, <0 error). Both are
    // POD facilities (CAP_SPAWN / CAP_DIRS); seeds leave them unused.
    int (*spawn)(const char *path, int argc, const char *const *argv);
    int (*mkdir)(const char *path);

    // --- console: the VDU control stream (CAP_CONSOLE) ---------------------
    // Feed one byte to the VDU driver, the same path as BASIC's VDU statement:
    // a full-screen program positions the cursor (VDU 31,x,y), sets colours
    // (VDU 17,c), clears (VDU 12) and shapes the cursor (VDU 23,1,...), and
    // prints text (bytes >= 32) through it. This is what a text-mode editor
    // draws with; plain putc/puts do NOT interpret these control codes.
    void (*vdu)(int b);

    // --- console: text screen size (CAP_CONSOLE) ---------------------------
    // The current text grid in character cells (columns x rows), tracking the
    // live resolution (BASIC's SCREEN changes it). Either pointer may be 0. A
    // full-screen program reads this instead of assuming 80x25.
    void (*screen_size)(int *cols, int *rows);

    // --- console font, drawn in graphics mode (CAP_GRAPHICS) ---------------
    // The console's own bitmap font, for a POD that wants to render text exactly
    // the way the system does (a full-screen editor, say) while drawing through
    // the graphics surface, so it composes with the back buffer and never flickers.
    // con_font reports the character cell size in pixels; con_glyph fills one cell
    // at pixel (px,py) with `bg` and draws glyph `ch` (0..255) in `fg`. 0xRRGGBB.
    void (*con_font)(int *w, int *h);
    void (*con_glyph)(int px, int py, int ch, unsigned fg, unsigned bg);

    // --- directory browsing (CAP_DIRS) -------------------------------------
    // Enumerate and navigate directories, so a POD (a file dialog, say) can
    // browse the card. One scan is active at a time: dir_open starts it, then
    // dir_read yields each entry until it returns 0. getcwd/chdir report and set
    // the current directory. These back the pod-libc <dirent.h> and getcwd/chdir.
    int (*dir_open)(const char *path);                    // 1 ok, 0 could not open
    int (*dir_read)(char *name, int namesz, int *is_dir, long *size);  // 1 entry, 0 end
    int (*getcwd)(char *buf, int sz);                     // -> length, or <0 on error
    int (*chdir)(const char *path);                       // 0 ok, <0 error

    // --- system clipboard (CAP_CONSOLE) ------------------------------------
    // One shared buffer in the machine (not the POD), so text copied here can be
    // pasted at the BASIC prompt or in the next program that runs. clip_get fills
    // buf and returns the FULL length (so the caller can tell it was truncated).
    void (*clip_set)(const char *data, int len);
    int  (*clip_get)(char *buf, int max);
    int  (*clip_len)(void);

    // v20: make a range of freshly-written bytes executable -- clean the data
    // cache to the point of unification and invalidate the matching instruction
    // cache lines. A program that copies or generates code (for example a POD
    // that loads and runs a seed) calls this on the bytes before it jumps into
    // them, since the D- and I-caches are not coherent on ARM. (Under CAP_HEAP,
    // the companion of the aligned allocation the code lands in.)
    void (*icache_sync)(const void *addr, unsigned long size);

    // v21: fast paletted blit (CAP_GRAPHICS). Blit a w*h 8-bit indexed image
    // through a 256-entry 0xRRGGBB palette to the graphics surface, nearest-
    // neighbour scaled by `scale`, top-left at (dx,dy), clipped to the screen.
    // One call pushes a whole frame (a game's 320x200) instead of tens of
    // thousands of putpixels; it composes on the back buffer like the other gfx_*.
    void (*gfx_blit8)(const unsigned char *idx, const unsigned int *pal,
                      int w, int h, int dx, int dy, int scale);

    // v22: keys currently held down (CAP_CONSOLE), for games that need hold-to-
    // move rather than one-shot presses. Fills out[] (up to max) with BerryBasiC
    // key codes and returns the count, or -1 when there is no USB keyboard (a
    // serial terminal reports presses, not held state). Poll a key first (inkey)
    // so the underlying HID report is fresh.
    int (*keys_down)(int *out, int max);

    // v23: streamed PCM audio (CAP_CONSOLE), for sampled sound / games. A stereo
    // sample stream played by DMA into the analogue jack with no ongoing CPU
    // cost (real hardware only; discards silently on QEMU/host). audio_open sets
    // the sample rate and starts; audio_avail reports free stereo frames so the
    // caller mixes just enough each frame; audio_write feeds 16-bit sample pairs.
    int  (*audio_open)(int rate);                     // 0 ok, <0 fail
    int  (*audio_avail)(void);                        // free stereo frames
    int  (*audio_write)(const short *stereo, int frames);  // -> frames accepted
    void (*audio_close)(void);
} BerryServices;

// The one services pointer the berry-libc reads. A POD's crt0 sets it before
// main(); a seed's SEED_EXPORT/SEED_KEYWORD entry trampoline sets it before the
// body runs. Every library routine that reaches the machine (malloc, printf,
// fopen, ...) goes through it, so seeds and PODs share exactly one C library.
extern const BerryServices *berry_svc;

#endif // BERRY_SERVICES_H
