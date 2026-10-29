/* grow - the BerryBasiC build system. "You plant seeds, you grow pods."
 *
 * grow reads a plain-text GROW file (declarative: `project`, `packet` and `pod`
 * blocks with indented properties) and builds .PKT packets and .POD executables
 * from C source, ON the machine. It does not compile anything itself: it works
 * out the build order and spawns the on-card tcc for each compile / packet /
 * link step, then reads back the resulting POD's capabilities.
 *
 * See doc "GROW - the BerryBasiC Build System". This implements the core:
 * parse, resolve the use-graph (packets before their consumers), build, derive
 * and check capabilities. Deferred (and noted): wildcards, timestamp-based
 * incremental rebuilds, header-dependency scanning, `info`/`install`/`new`,
 * parallel `-j`, and sha/sign seals.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pod.h>

POD_NAME("grow")
POD_DESCRIPTION("the BerryBasiC build system")
POD_NEEDS(CAP_CONSOLE | CAP_FILES | CAP_HEAP | CAP_DIRS | CAP_SPAWN,
          "CONSOLE=progress; FILES=sources and outputs; HEAP=working memory; "
          "DIRS=the output directory; SPAWN=runs tcc for each step")

extern const BerryServices *berry_svc;   /* stashed by the pod-libc crt0 */

#define TCC_PATH   "/sys/TCC.POD"
#define GROW_PATH  "/sys/GROW.POD"    /* grow recurses into a subdir by spawning itself */
#define MAX_TARGET 32
#define VAL        1024

typedef struct {
    int  line;
    char kind;                 /* 'K' packet, 'P' pod */
    char name[16];
    char source[VAL];
    char use[256], needs[128], include[256], define[256], option[256];
    char title[64], version[32], author[32], about[128], license[32];
    char out[40];
    int  built;
} Target;

static Target targets[MAX_TARGET];
static int    ntarget;

/* Sub-projects: each named dir holds its own GROW file. A collecting GROW lists
   them with `subdir NAME`, and grow builds each by recursing into it. */
static char subdirs[MAX_TARGET][40];
static int  nsubdir;

static char proj_out[40]      = "build";
static char proj_includes[256]= "";
static char proj_packets[256] = "/sys/lib";
static char proj_define[256]  = "";
static char proj_option[256]  = "";

static struct { char name[24], val[160]; } vars[32];
static int nvars;

static const char *grow_file = "GROW";
static int opt_verbose, opt_dryrun;
static int had_error;

/* ------------------------------------------------------------------ util */
static void die(const char *msg, const char *arg) {
    printf("grow: %s", msg);
    if (arg) { printf(": %s", arg); }
    printf("\n");
    exit(1);
}
static void errline(int line, const char *msg, const char *arg) {
    printf("%s:%d: %s", grow_file, line, msg);
    if (arg) printf(" '%s'", arg);
    printf("\n");
    had_error = 1;
}
static int  is_ws(int c) { return c == ' ' || c == '\t'; }
static void lower(char *s) { for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32; }
static void append(char *dst, int cap, const char *src) {   /* space-join */
    int n = (int)strlen(dst);
    if (n && n < cap - 1) dst[n++] = ' ';
    while (*src && n < cap - 1) dst[n++] = *src++;
    dst[n] = 0;
}
/* basename minus extension, into out (bounded, and rejected if too long for 8.3) */
static void base_noext(const char *path, char *out, int outsz, int line) {
    const char *b = path;
    for (const char *q = path; *q; q++) if (*q == '/') b = q + 1;
    int n = 0;
    for (; b[n] && b[n] != '.' && n < outsz - 1; n++) out[n] = b[n];
    out[n] = 0;
    if (n > 8) errline(line, "name too long for the card", b);
}

/* ------------------------------------------------------------- variables */
static const char *var_get(const char *name) {
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, name)) return vars[i].val;
    return 0;
}
static void var_set(const char *name, const char *val) {
    for (int i = 0; i < nvars; i++) if (!strcmp(vars[i].name, name)) { strncpy(vars[i].val, val, 159); return; }
    if (nvars < 32) { strncpy(vars[nvars].name, name, 23); strncpy(vars[nvars].val, val, 159); nvars++; }
}
/* single-pass textual $VAR substitution of `in` into `out` */
static void subst(const char *in, char *out, int outsz) {
    int o = 0;
    while (*in && o < outsz - 1) {
        if (*in == '$' && (in[1] == '_' || (in[1] >= 'A' && in[1] <= 'Z') || (in[1] >= 'a' && in[1] <= 'z'))) {
            char nm[24]; int k = 0; in++;
            while ((*in == '_' || (*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') ||
                    (*in >= '0' && *in <= '9')) && k < 23) nm[k++] = *in++;
            nm[k] = 0;
            const char *v = var_get(nm);
            if (v) while (*v && o < outsz - 1) out[o++] = *v++;
        } else out[o++] = *in++;
    }
    out[o] = 0;
}

/* -------------------------------------------------------------- the parser */
/* Strip a trailing # comment that is not inside double quotes. */
static void strip_comment(char *s) {
    int q = 0;
    for (; *s; s++) {
        if (*s == '"') q = !q;
        else if (*s == '#' && !q) { *s = 0; return; }
    }
}
/* De-quote a scalar value in place (whole value wrapped in "..."). */
static void dequote(char *s) {
    int n = (int)strlen(s);
    if (n >= 2 && s[0] == '"' && s[n-1] == '"') { memmove(s, s + 1, n - 2); s[n-2] = 0; }
}

static Target *cur_target;   /* block being filled, or 0 for project */
static int     in_project;

static void set_prop(int line, const char *key, char *val) {
    dequote(val);
    if (in_project) {
        if      (!strcmp(key, "out"))      strncpy(proj_out, val, 39);
        else if (!strcmp(key, "includes")) append(proj_includes, 256, val);
        else if (!strcmp(key, "packets"))  { proj_packets[0] = 0; append(proj_packets, 256, val); }
        else if (!strcmp(key, "define"))   append(proj_define, 256, val);
        else if (!strcmp(key, "option"))   append(proj_option, 256, val);
        else errline(line, "unknown project property", key);
        return;
    }
    Target *t = cur_target;
    if (!t) { errline(line, "property outside a block", key); return; }
    if      (!strcmp(key, "source"))  append(t->source, VAL, val);
    else if (!strcmp(key, "include")) append(t->include, 256, val);
    else if (!strcmp(key, "define"))  append(t->define, 256, val);
    else if (!strcmp(key, "option"))  append(t->option, 256, val);
    else if (!strcmp(key, "needs"))   append(t->needs, 128, val);
    else if (!strcmp(key, "out"))     strncpy(t->out, val, 39);
    else if (!strcmp(key, "about"))   strncpy(t->about, val, 127);
    else if (!strcmp(key, "use")) {
        if (t->kind != 'P') { errline(line, "'use' is not valid in a packet block", 0); return; }
        append(t->use, 256, val);
    }
    else if (!strcmp(key, "title"))   { if (t->kind != 'P') goto podonly; strncpy(t->title, val, 63); }
    else if (!strcmp(key, "version")) { if (t->kind != 'P') goto podonly; strncpy(t->version, val, 31); }
    else if (!strcmp(key, "author"))  { if (t->kind != 'P') goto podonly; strncpy(t->author, val, 31); }
    else if (!strcmp(key, "license")) { if (t->kind != 'P') goto podonly; strncpy(t->license, val, 31); }
    else if (!strcmp(key, "kind") || !strcmp(key, "entry") ||
             !strcmp(key, "stack") || !strcmp(key, "heap") || !strcmp(key, "seal")) {
        /* accepted but handled by tcc defaults in this build */
    }
    else if (!strcmp(key, "sources")) errline(line, "unknown property (did you mean 'source'?)", key);
    else errline(line, "unknown property", key);
    return;
podonly:
    errline(line, "that property belongs on a pod, not a packet", key);
}

static void new_block(int line, const char *kw, const char *name) {
    in_project = 0; cur_target = 0;
    if (!strcmp(kw, "project")) { in_project = 1; return; }
    if (strcmp(kw, "pod") && strcmp(kw, "packet") && strcmp(kw, "seed"))
        { errline(line, "unknown block keyword", kw); return; }
    if (ntarget >= MAX_TARGET) { errline(line, "too many targets", 0); return; }
    Target *t = &targets[ntarget++];
    memset(t, 0, sizeof *t);
    t->line = line;
    t->kind = !strcmp(kw, "packet") ? 'K' : !strcmp(kw, "seed") ? 'S' : 'P';
    strncpy(t->name, name, 15);
    cur_target = t;
}

static void parse(char *text) {
    int line = 0;
    char *p = text;
    while (*p) {
        char raw[VAL]; int r = 0;
        /* gather one logical line (honour \ continuation) */
        for (;;) {
            line++;
            while (*p && *p != '\n' && r < VAL - 1) raw[r++] = *p++;
            if (*p == '\n') p++;
            raw[r] = 0;
            /* trailing backslash -> continue onto the next physical line */
            int e = r; while (e > 0 && is_ws(raw[e-1])) e--;
            if (e > 0 && raw[e-1] == '\\') { r = e - 1; continue; }
            break;
        }
        strip_comment(raw);
        char subd[VAL]; subst(raw, subd, VAL);

        /* trim */
        char *s = subd; while (is_ws(*s)) s++;
        int indented = (s != subd);
        int n = (int)strlen(s); while (n > 0 && is_ws(s[n-1])) s[--n] = 0;
        if (!*s) continue;

        /* split first word (the key/keyword) from the rest */
        char *rest = s; while (*rest && !is_ws(*rest)) rest++;
        char kw[24]; int kn = (int)(rest - s); if (kn > 23) kn = 23;
        memcpy(kw, s, kn); kw[kn] = 0;
        while (is_ws(*rest)) rest++;

        if (indented) { char k[24]; strncpy(k, kw, 23); lower(k); set_prop(line, k, rest); continue; }

        char kwl[24]; strncpy(kwl, kw, 23); kwl[23] = 0; lower(kwl);
        if (!strcmp(kwl, "set")) {
            char *nm = rest; while (*rest && !is_ws(*rest)) rest++;
            char name[24]; int ln = (int)(rest - nm); if (ln > 23) ln = 23;
            memcpy(name, nm, ln); name[ln] = 0;
            while (is_ws(*rest)) rest++;
            char v[160]; dequote(rest); strncpy(v, rest, 159); v[159] = 0;
            if (!var_get(name)) var_set(name, v);   /* -s overrides win (set earlier) */
            continue;
        }
        if (!strcmp(kwl, "subdir")) {
            char nm[40]; strncpy(nm, rest, 39); nm[39] = 0;
            char *sp = nm; while (*sp && !is_ws(*sp)) sp++; *sp = 0;   /* first token */
            if (nm[0] && nsubdir < MAX_TARGET) { strncpy(subdirs[nsubdir], nm, 39); nsubdir++; }
            continue;
        }
        /* a block header: the name keeps its case */
        char name[16]; strncpy(name, rest, 15); name[15] = 0;
        char *sp = name; while (*sp && !is_ws(*sp)) sp++; *sp = 0;
        new_block(line, kwl, name);
    }
}

/* ----------------------------------------------------- spawning the compiler */
static char  argbuf[8192];
static int   argpos;
static char *cargv[128];
static int   cargc;
static void arg_reset(void) { argpos = 0; cargc = 0; }
static void arg1(const char *s) {
    cargv[cargc++] = argbuf + argpos;
    while (*s) argbuf[argpos++] = *s++;
    argbuf[argpos++] = 0;
}
static void arg2(const char *pfx, const char *s) {   /* one arg: pfx+s (e.g. -Iinc) */
    cargv[cargc++] = argbuf + argpos;
    while (*pfx) argbuf[argpos++] = *pfx++;
    while (*s) argbuf[argpos++] = *s++;
    argbuf[argpos++] = 0;
}
/* add each whitespace token of `list`, each prefixed by pfx ("" for none) */
static void arg_split(const char *list, const char *pfx) {
    const char *s = list;
    while (*s) {
        while (is_ws(*s)) s++;
        if (!*s) break;
        const char *st = s; while (*s && !is_ws(*s)) s++;
        cargv[cargc] = argbuf + argpos;
        for (const char *q = pfx; *q; q++) argbuf[argpos++] = *q;
        while (st < s) argbuf[argpos++] = *st++;
        argbuf[argpos++] = 0;
        cargc++;
    }
}
static int spawn_tcc(void) {
    if (opt_verbose || opt_dryrun) {
        printf("      +");
        for (int i = 0; i < cargc; i++) printf(" %s", cargv[i]);
        printf("\n");
    }
    if (opt_dryrun) return 0;
    return berry_svc->spawn(TCC_PATH, cargc, (const char *const *)cargv);
}

/* common include/define/option flags for a target */
static void add_flags(Target *t) {
    arg_split(proj_includes, "-I");
    if (t) arg_split(t->include, "-I");
    arg1("-Iinc");                            /* the conventional project headers */
    arg_split(proj_define, "-D");
    if (t) arg_split(t->define, "-D");
    arg_split(proj_option, "");
    if (t) arg_split(t->option, "");
}

/* compile one source to <out>/<base>.o ; returns 0 ok */
static int compile(const char *src, char *objout, int objsz, Target *t) {
    char base[16]; base_noext(src, base, sizeof base, t->line);
    snprintf(objout, objsz, "%s/%s.o", proj_out, base);
    printf("    compile  %s\n", src);
    arg_reset();
    arg1("tcc"); arg1("-c"); arg1(src); arg1("-o"); arg1(objout);
    add_flags(t);
    int rc = spawn_tcc();
    if (rc != 0) { printf("grow: %s: compilation failed\n", src); had_error = 1; }
    return rc;
}

/* ---------------------------------------------------------- capabilities */
static const struct { unsigned long long bit; const char *name; } capnames[] = {
    {1ull<<0,"CONSOLE"},{1ull<<1,"VARS"},{1ull<<2,"FILES"},{1ull<<3,"DIRS"},
    {1ull<<4,"GRAPHICS"},{1ull<<5,"SOUND"},{1ull<<6,"GPIO"},{1ull<<7,"I2C"},
    {1ull<<8,"TIME"},{1ull<<9,"HEAP"},{1ull<<10,"KEYWORD"},{1ull<<11,"SPAWN"},
    {1ull<<12,"RAWMEM"},
};
static unsigned long long read_pod_caps(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[64];
    if (fread(h, 1, 64, f) != 64) { fclose(f); return 0; }
    fclose(f);
    unsigned long long c = 0;
    for (int i = 0; i < 8; i++) c |= (unsigned long long)h[44 + i] << (8 * i);
    return c;
}
static int cap_bit(const char *name) {
    for (unsigned i = 0; i < sizeof capnames / sizeof capnames[0]; i++)
        if (!strcmp(capnames[i].name, name)) return (int)i;  /* index; bit = capnames[i].bit */
    return -1;
}
/* Assert derived caps are all listed in `needs` (the useful direction). */
static void check_needs(Target *t, unsigned long long derived) {
    if (!t->needs[0]) return;
    unsigned long long declared = 0;
    const char *s = t->needs;
    while (*s) {
        while (is_ws(*s)) s++;
        if (!*s) break;
        char nm[16]; int k = 0; const char *st = s;
        while (*s && !is_ws(*s)) s++;
        for (const char *q = st; q < s && k < 15; q++) nm[k++] = *q;
        nm[k] = 0;
        int bi = cap_bit(nm);
        if (bi >= 0) declared |= capnames[bi].bit;
    }
    for (unsigned i = 0; i < sizeof capnames / sizeof capnames[0]; i++)
        if ((derived & capnames[i].bit) && !(declared & capnames[i].bit)) {
            printf("grow: %s: uses CAP_%s but 'needs' does not list it\n", t->name, capnames[i].name);
            had_error = 1;
        }
}
static void print_caps(unsigned long long c) {
    printf("    caps    ");
    int any = 0;
    for (unsigned i = 0; i < sizeof capnames / sizeof capnames[0]; i++)
        if (c & capnames[i].bit) { printf(" %s", capnames[i].name); any = 1; }
    if (!any) printf(" (none)");
    printf("\n");
}

/* -------------------------------------------------------- building targets */
static Target *find_target(const char *name, char kind) {
    for (int i = 0; i < ntarget; i++)
        if (targets[i].kind == kind && !strcmp(targets[i].name, name)) return &targets[i];
    return 0;
}
static Target *find_any(const char *name) {
    for (int i = 0; i < ntarget; i++) if (!strcmp(targets[i].name, name)) return &targets[i];
    return 0;
}
static void out_path(Target *t, const char *ext, char *buf, int sz) {
    if (t->out[0]) snprintf(buf, sz, "%s/%s", proj_out, t->out);
    else           snprintf(buf, sz, "%s/%s.%s", proj_out, t->name, ext);
}

static int build_target(Target *t);   /* fwd */

static int build_packet(Target *t) {
    if (!t->source[0]) { printf("grow: %s: a packet needs a 'source'\n", t->name); had_error = 1; return -1; }
    printf("  packet %s\n", t->name);
    char pkt[64]; out_path(t, "PKT", pkt, sizeof pkt);
    /* compile each source (one file == one member), then pack the objects */
    char objs[16][64]; int nobj = 0;
    const char *s = t->source; char srcs[32][64]; int nsrc = 0;
    while (*s) { while (is_ws(*s)) s++; if (!*s) break; const char *st = s; while (*s && !is_ws(*s)) s++;
                 int l = (int)(s - st); if (l > 63) l = 63; memcpy(srcs[nsrc], st, l); srcs[nsrc][l] = 0; if (nsrc < 31) nsrc++; }
    for (int i = 0; i < nsrc; i++) {
        if (compile(srcs[i], objs[nobj], 64, t) != 0) return -1;
        nobj++;
    }
    arg_reset(); arg1("tcc"); arg1("-pkt"); arg1(pkt);
    for (int i = 0; i < nobj; i++) arg1(objs[i]);
    int rc = spawn_tcc();
    if (rc != 0) { printf("grow: %s: packing failed\n", t->name); had_error = 1; return -1; }
    printf("    packet   %s\n", pkt);
    t->built = 1;
    return 0;
}

static int build_pod(Target *t) {
    if (!t->source[0]) { printf("grow: %s: a pod needs a 'source'\n", t->name); had_error = 1; return -1; }
    printf("  pod %s\n", t->name);

    /* build any same-file packets this pod uses, first */
    char used[16][64]; int nused = 0;   /* resolved link inputs (file paths or -l) */
    {
        const char *s = t->use;
        while (*s) {
            while (is_ws(*s)) s++; if (!*s) break;
            char nm[16]; int k = 0; const char *st = s; while (*s && !is_ws(*s)) s++;
            for (const char *q = st; q < s && k < 15; q++) nm[k++] = *q; nm[k] = 0;
            Target *p = find_target(nm, 'K');
            if (p) {
                if (!p->built && build_target(p) != 0) return -1;
                out_path(p, "PKT", used[nused], 64);       /* link the built packet file */
            } else {
                snprintf(used[nused], 64, "\x01%s", nm);   /* marker: system packet -> -l */
            }
            if (nused < 15) nused++;
        }
    }

    /* compile sources */
    char objs[32][64]; int nobj = 0;
    const char *s = t->source;
    while (*s) {
        while (is_ws(*s)) s++; if (!*s) break; const char *st = s; while (*s && !is_ws(*s)) s++;
        char src[64]; int l = (int)(s - st); if (l > 63) l = 63; memcpy(src, st, l); src[l] = 0;
        if (compile(src, objs[nobj], 64, t) != 0) return -1;
        if (nobj < 31) nobj++;
    }

    /* A pod that uses CORE is a main()-based pod-libc program: the entry lives
       in crt0.o, which the closure would not pull on its own, so link it first.
       SOFTFP.O is the 128-bit soft-float runtime; it is shipped as a plain object
       (not a CORE.PKT member) because the on-machine packet linker loops on its
       symbol table, and printf/double math reach it, so force it in too. */
    int uses_core = 0;
    for (const char *u = t->use; *u; u++)
        if ((u[0]=='C'||u[0]=='c') && (u[1]=='O'||u[1]=='o') && (u[2]=='R'||u[2]=='r') && (u[3]=='E'||u[3]=='e')) { uses_core = 1; break; }

    /* link the POD */
    char pod[64]; out_path(t, "POD", pod, sizeof pod);
    arg_reset(); arg1("tcc"); arg1("-pod");
    if (uses_core) { arg1("/sys/lib/CRT0.O"); arg1("/sys/lib/SOFTFP.O"); }
    for (int i = 0; i < nobj; i++) arg1(objs[i]);
    for (int i = 0; i < nused; i++) {
        if (used[i][0] == 1) arg2("-l", used[i] + 1);      /* system packet */
        else arg1(used[i]);                                /* same-file packet file */
    }
    /* library search path for system packets */
    arg_split(proj_packets, "-L");
    arg1("-o"); arg1(pod);
    add_flags(t);
    printf("    link     %s\n", t->use[0] ? t->use : "(no packets)");
    int rc = spawn_tcc();
    if (rc != 0) { printf("grow: %s: link failed\n", t->name); had_error = 1; return -1; }

    unsigned long long caps = read_pod_caps(pod);
    print_caps(caps);
    check_needs(t, caps);
    printf("    pod      %s\n", pod);
    t->built = 1;
    return 0;
}

static int build_seed(Target *t) {
    if (!t->source[0]) { printf("grow: %s: a seed needs a 'source'\n", t->name); had_error = 1; return -1; }
    printf("  seed %s\n", t->name);
    char sed[64]; out_path(t, "SED", sed, sizeof sed);
    /* tcc -seed compiles and links every source in one position-independent pass
       into a flat blob. It also links the berry-libc packet SEEDCORE by closure
       (the seed analogue of a pod's `use CORE`): a self-contained seed pulls
       nothing and stays tiny, one that calls printf/malloc/<graphics.h> pulls
       just the members it needs. */
    arg_reset(); arg1("tcc"); arg1("-seed");
    const char *s = t->source;
    while (*s) {
        while (is_ws(*s)) s++; if (!*s) break; const char *st = s; while (*s && !is_ws(*s)) s++;
        char src[64]; int l = (int)(s - st); if (l > 63) l = 63; memcpy(src, st, l); src[l] = 0;
        printf("    compile  %s\n", src);
        arg1(src);
    }
    arg2("-l", "SEEDCORE");
    arg_split(proj_packets, "-L");
    arg1("-o"); arg1(sed);
    add_flags(t);
    int rc = spawn_tcc();
    if (rc != 0) { printf("grow: %s: seed build failed\n", t->name); had_error = 1; return -1; }
    printf("    seed     %s\n", sed);
    t->built = 1;
    return 0;
}

static int build_target(Target *t) {
    if (t->built) return 0;
    return t->kind == 'K' ? build_packet(t) : t->kind == 'S' ? build_seed(t) : build_pod(t);
}

/* ------------------------------------------------------------- commands */
static const char *kind_word(char k) { return k == 'K' ? "packet" : k == 'S' ? "seed" : "pod"; }
static const char *kind_ext(char k)  { return k == 'K' ? "PKT"    : k == 'S' ? "SED"  : "POD"; }

static void cmd_list(void) {
    printf("targets in %s:\n", grow_file);
    for (int i = 0; i < ntarget; i++) {
        Target *t = &targets[i];
        printf("  %-7s %-12s source: %s\n", kind_word(t->kind), t->name, t->source);
        if (t->use[0]) printf("                       use: %s\n", t->use);
    }
    for (int i = 0; i < nsubdir; i++)
        printf("  subdir  %s/  (its own GROW)\n", subdirs[i]);
}
static void cmd_clean(void) {
    for (int i = 0; i < ntarget; i++) {
        Target *t = &targets[i];
        char p[64]; out_path(t, kind_ext(t->kind), p, sizeof p);
        if (remove(p) == 0) printf("  remove   %s\n", p);
        /* objects */
        const char *s = t->source;
        while (*s) { while (is_ws(*s)) s++; if (!*s) break; const char *st = s; while (*s && !is_ws(*s)) s++;
                     char src[64]; int l = (int)(s-st); if (l>63) l=63; memcpy(src, st, l); src[l]=0;
                     char base[16]; base_noext(src, base, sizeof base, t->line);
                     char o[64]; snprintf(o, sizeof o, "%s/%s.o", proj_out, base); remove(o); }
    }
    printf("cleaned %s\n", proj_out);
}
/* Build the targets in THIS GROW file (all, or just `only`). */
static int build_locals(const char *only) {
    int n = 0;
    /* packets first (a pod's use pulls them in on demand too), then pods/seeds */
    for (int pass = 0; pass < 2 && !had_error; pass++)
        for (int i = 0; i < ntarget && !had_error; i++) {
            Target *t = &targets[i];
            if ((pass == 0) != (t->kind == 'K')) continue;
            if (only && strcmp(t->name, only)) continue;
            if (!t->built) { if (build_target(t) == 0) n++; }
        }
    if (n) printf("built %d target%s\n", n, n == 1 ? "" : "s");
    return n;
}

static int is_subdir(const char *name) {
    for (int i = 0; i < nsubdir; i++) if (!strcmp(subdirs[i], name)) return 1;
    return 0;
}

/* Recurse into a sub-project: enter its directory and run grow there (a fresh
   process that reads that dir's own GROW and builds it), then come back. */
static int build_subdir(const char *dir) {
    char cwd[128]; int cl = berry_svc->getcwd ? berry_svc->getcwd(cwd, sizeof cwd) : -1;
    printf("--- %s ---\n", dir);
    if (!berry_svc->chdir || berry_svc->chdir(dir) != 0) {
        printf("grow: cannot enter %s/\n", dir); had_error = 1; return -1;
    }
    const char *av[4]; int ac = 0;
    av[ac++] = "grow";
    if (opt_verbose) av[ac++] = "-v";
    if (opt_dryrun)  av[ac++] = "-n";
    int rc = opt_dryrun ? 0 : berry_svc->spawn(GROW_PATH, ac, av);
    if (cl >= 0) berry_svc->chdir(cwd); else berry_svc->chdir("..");
    if (rc != 0) { printf("grow: subdir %s failed\n", dir); had_error = 1; return -1; }
    return 0;
}

/* Build a selection (target and/or subdir names), or everything if none named. */
static void cmd_grow(char **names, int nnames) {
    if (ntarget > 0 && berry_svc->mkdir) berry_svc->mkdir(proj_out);   /* output dir */
    if (nnames == 0) {
        build_locals(0);
        for (int i = 0; i < nsubdir && !had_error; i++) build_subdir(subdirs[i]);
    } else {
        for (int i = 0; i < nnames && !had_error; i++) {
            if (find_any(names[i]))       build_locals(names[i]);
            else if (is_subdir(names[i])) build_subdir(names[i]);
            else { printf("grow: no target or subdir named '%s'\n", names[i]); had_error = 1; }
        }
    }
    if (had_error) { printf("grow: build failed\n"); exit(1); }
}

/* ------------------------------------------------ scaffolding (new/add/use) */
static int  file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) { fclose(f); return 1; } return 0; }
static int  read_all(const char *p, char *buf, int sz) {
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    int n = (int)fread(buf, 1, sz - 1, f); fclose(f); if (n < 0) n = 0; buf[n] = 0; return n;
}
static void write_all(const char *p, const char *data, int len) {
    FILE *f = fopen(p, "wb"); if (!f) die("cannot write", p);
    fwrite(data, 1, len, f); fclose(f);
}
static void upper_name(const char *in, char *out, int sz) {
    int i = 0; for (; in[i] && i < sz - 1; i++) { char c = in[i]; out[i] = (c >= 'a' && c <= 'z') ? c - 32 : c; }
    out[i] = 0;
}
static int is_ident(const char *s) {
    if (!s || !*s) return 0;
    char c = *s; if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_')) return 0;
    for (const char *p = s + 1; *p; p++) { c = *p;
        if (!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_')) return 0; }
    return 1;
}
/* A starter source for a fresh target: an entry stub matching the kind. */
static void make_entry_stub(char kind, const char *name, char *buf, int sz) {
    if (kind == 'P')
        snprintf(buf, sz,
            "#include <stdio.h>\n#include <pod.h>\n\n"
            "POD_NAME(\"%s\")\n"
            "POD_NEEDS(CAP_CONSOLE, \"CONSOLE=greeting\")\n\n"
            "int main(int argc, char **argv) {\n"
            "    (void)argc; (void)argv;\n"
            "    printf(\"hello from %s\\n\");\n"
            "    return 0;\n}\n", name, name);
    else if (kind == 'S')
        snprintf(buf, sz,
            "#include \"seed.h\"\n\n"
            "// r = %s(x)  after this .SED is loaded with SEED / called with CALL.\n"
            "SEED_EXPORT(%s) {\n"
            "    (void)svc; (void)argc;\n"
            "    return argc > 0 ? argv[0].num : 0;\n}\n", name, name);
    else
        snprintf(buf, sz,
            "// %s packet: one source file == one packet member. Add more with\n"
            "//   grow add %s FILE.C\n"
            "int %s_value(void) {\n"
            "    return 42;\n}\n", name, name, name);
}

/* Insert "    KEY  VAL" as the first property of the block named `name`.
   Returns 0 on success, -1 if no such block. */
static int grow_add_prop(const char *name, const char *key, const char *val) {
    static char text[65536], out[65536];
    int n = read_all(grow_file, text, sizeof text);
    if (n < 0) die("no build file", grow_file);
    /* find the block header line "pod|seed|packet <name>" at column 0 */
    char *at = 0, *p = text;
    while (*p) {
        char *eol = p; while (*eol && *eol != '\n') eol++;
        if (*p != ' ' && *p != '\t' && *p != '#' && *p != '\n') {
            char *q = p; char kw[12]; int k = 0;
            while (q < eol && *q != ' ' && *q != '\t') { if (k < 11) kw[k++] = *q; q++; }
            kw[k] = 0;
            if (!strcmp(kw, "pod") || !strcmp(kw, "seed") || !strcmp(kw, "packet")) {
                while (q < eol && (*q == ' ' || *q == '\t')) q++;
                char nm[16]; int j = 0;
                while (q < eol && *q != ' ' && *q != '\t') { if (j < 15) nm[j++] = *q; q++; }
                nm[j] = 0;
                if (!strcmp(nm, name)) { at = (*eol ? eol + 1 : eol); break; }
            }
        }
        p = (*eol ? eol + 1 : eol);
    }
    if (!at) return -1;
    int off = (int)(at - text);
    char line[160]; int ll = snprintf(line, sizeof line, "    %-7s%s\n", key, val);
    if (n + ll >= (int)sizeof out) die("build file too large", grow_file);
    memcpy(out, text, off);
    memcpy(out + off, line, ll);
    memcpy(out + off + ll, text + off, n - off);
    write_all(grow_file, out, n + ll);
    return 0;
}

static void cmd_new(const char *kinds, const char *name) {
    if (!kinds || !name) die("usage: grow new pod|seed|packet NAME", 0);
    char kind = !strcmp(kinds, "pod") ? 'P' : !strcmp(kinds, "seed") ? 'S'
              : !strcmp(kinds, "packet") ? 'K' : 0;
    if (!kind) { printf("grow new: kind must be pod, seed or packet (not '%s')\n", kinds); exit(1); }
    if (!is_ident(name)) { printf("grow new: '%s' must be a C identifier (letter, then letters/digits/_)\n", name); exit(1); }

    static char text[65536], out[65536];
    int n = read_all(grow_file, text, sizeof text);
    if (n > 0) { parse(text); if (find_any(name)) { printf("grow: a target named '%s' already exists in %s\n", name, grow_file); exit(1); } }

    char up[24]; upper_name(name, up, sizeof up);
    char srcfile[28]; snprintf(srcfile, sizeof srcfile, "%s.C", up);
    if (!file_exists(srcfile)) {
        static char stub[1024]; make_entry_stub(kind, name, stub, sizeof stub);
        write_all(srcfile, stub, (int)strlen(stub));
        printf("  create   %s\n", srcfile);
    } else printf("  reuse    %s (already exists)\n", srcfile);

    int off = 0;
    if (n <= 0) off += snprintf(out, sizeof out, "# GROW build file\nproject\n    out    build\n");
    else { memcpy(out, text, n); off = n; if (off && out[off-1] != '\n') out[off++] = '\n'; }
    off += snprintf(out + off, sizeof out - off, "\n%s %s\n    source %s\n", kinds, name, srcfile);
    if (kind == 'P') off += snprintf(out + off, sizeof out - off, "    use    CORE\n    needs  CONSOLE\n");
    write_all(grow_file, out, off);
    if (n <= 0) printf("  create   %s\n", grow_file);
    printf("  %-7s %s  ->  %s/%s.%s\n", kinds, name, proj_out, name, kind_ext(kind));
    printf("Next: edit %s, then 'grow %s' to build it.\n", srcfile, name);
}

static void cmd_add(const char *name, char **files, int nfiles) {
    if (!name || nfiles < 1) die("usage: grow add TARGET FILE...", 0);
    static char text[65536];
    int n = read_all(grow_file, text, sizeof text);
    if (n < 0) die("no build file", grow_file);
    parse(text);
    Target *t = find_any(name);
    if (!t) { printf("grow: no target named '%s' in %s\n", name, grow_file); exit(1); }
    for (int i = 0; i < nfiles; i++) {
        if (!file_exists(files[i])) {
            char stub[128];
            snprintf(stub, sizeof stub, "// %s - part of %s (%s)\n", files[i], name, kind_word(t->kind));
            write_all(files[i], stub, (int)strlen(stub));
            printf("  create   %s\n", files[i]);
        } else printf("  reuse    %s\n", files[i]);
        if (grow_add_prop(name, "source", files[i]) != 0) { printf("grow: could not find block '%s'\n", name); exit(1); }
        printf("  source   %s  ->  %s\n", files[i], name);
    }
}

static void cmd_use(const char *pod, const char *pkt) {
    if (!pod || !pkt) die("usage: grow use POD PACKET", 0);
    static char text[65536];
    int n = read_all(grow_file, text, sizeof text);
    if (n < 0) die("no build file", grow_file);
    parse(text);
    Target *t = find_any(pod);
    if (!t) { printf("grow: no target named '%s' in %s\n", pod, grow_file); exit(1); }
    if (t->kind != 'P') { printf("grow: 'use' applies to a pod; '%s' is a %s\n", pod, kind_word(t->kind)); exit(1); }
    if (grow_add_prop(pod, "use", pkt) != 0) { printf("grow: could not find pod '%s'\n", pod); exit(1); }
    printf("  use      %s  ->  %s\n", pkt, pod);
}

/* Print a .SED blob's header (magic, version, entry, footprint). */
static void seed_info(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    unsigned char h[16]; int nr = (int)fread(h, 1, 16, f);
    long sz = 0; if (fseek(f, 0, 2) == 0) sz = ftell(f);
    fclose(f);
    if (nr < 16) return;
    unsigned magic = h[0]|(h[1]<<8)|(h[2]<<16)|((unsigned)h[3]<<24);
    unsigned ver = h[4]|(h[5]<<8), flags = h[6]|(h[7]<<8);
    unsigned entry = h[8]|(h[9]<<8)|(h[10]<<16)|((unsigned)h[11]<<24);
    unsigned img = h[12]|(h[13]<<8)|(h[14]<<16)|((unsigned)h[15]<<24);
    printf("    magic    %s   abi %u%s\n", magic == 0x44454553u ? "SEED" : "(bad)", ver,
           (flags & 1) ? "   keyword" : "");
    printf("    entry    0x%x   file %ld B   footprint %u B\n", entry, sz, img);
}

static void cmd_info(const char *name) {
    if (!name) {
        printf("targets in %s:\n", grow_file);
        for (int i = 0; i < ntarget; i++) {
            Target *t = &targets[i];
            char out[64]; out_path(t, kind_ext(t->kind), out, sizeof out);
            printf("  %-7s %-12s %s\n", kind_word(t->kind), t->name,
                   file_exists(out) ? "[built]" : "[not built]");
        }
        return;
    }
    Target *t = find_any(name);
    if (!t) { printf("grow: no target named '%s' in %s\n", name, grow_file); exit(1); }
    printf("%s %s\n", kind_word(t->kind), t->name);
    printf("  source   %s\n", t->source);
    if (t->use[0])   printf("  use      %s\n", t->use);
    if (t->needs[0]) printf("  needs    %s\n", t->needs);
    char out[64]; out_path(t, kind_ext(t->kind), out, sizeof out);
    if (!file_exists(out)) { printf("  not built yet - run 'grow %s'\n", t->name); return; }
    printf("  built    %s\n", out);
    if (t->kind == 'P') print_caps(read_pod_caps(out));
    else if (t->kind == 'S') seed_info(out);
    else { arg_reset(); arg1("tcc"); arg1("-pkt"); arg1("list"); arg1(out); spawn_tcc(); }  /* packet members */
}

static void usage(void) {
    printf("grow - the BerryBasiC build system\n\n");
    printf("Build:\n");
    printf("  grow                    build every target (and every subdir)\n");
    printf("  grow NAME...            build the named targets / subdirs\n");
    printf("  grow list               list the targets and subdirs\n");
    printf("  grow info [NAME]        show a target and its built artifact\n");
    printf("  grow clean              remove build outputs\n\n");
    printf("Scaffold:\n");
    printf("  grow new pod NAME       create a pod    target + NAME.C\n");
    printf("  grow new seed NAME      create a seed   target + NAME.C\n");
    printf("  grow new packet NAME    create a packet target + NAME.C\n");
    printf("  grow add NAME FILE...   add source file(s) to a target\n");
    printf("  grow use POD PKT        make a pod link a packet\n\n");
    printf("Options:  -f FILE  -v (verbose)  -n (dry run)  -s NAME=VALUE\n");
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    char *pos[16]; int npos = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if      (!strcmp(a, "-v")) opt_verbose = 1;
        else if (!strcmp(a, "-n")) opt_dryrun = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "-f") && i + 1 < argc) grow_file = argv[++i];
        else if (!strcmp(a, "-s") && i + 1 < argc) {
            char *eq = strchr(argv[++i], '=');
            if (eq) { *eq = 0; var_set(argv[i], eq + 1); }
        } else if (a[0] != '-') { if (npos < 16) pos[npos++] = a; }
    }
    const char *command = npos > 0 ? pos[0] : 0;

    /* scaffolding: these create/edit files and handle a missing GROW themselves.
       They each need their arguments, so `grow new/add/use` with too few words
       falls through to the build path -- which lets a target or subdir named
       like a subcommand (e.g. the `add` seed example) still build. */
    if (command && !strcmp(command, "help")) { usage(); return 0; }
    if (command && !strcmp(command, "new") && npos >= 3) { cmd_new(pos[1], pos[2]); return 0; }
    if (command && !strcmp(command, "add") && npos >= 3) { cmd_add(pos[1], pos + 2, npos - 2); return 0; }
    if (command && !strcmp(command, "use") && npos >= 3) { cmd_use(pos[1], pos[2]); return 0; }

    /* everything else reads the GROW file */
    FILE *f = fopen(grow_file, "r");
    if (!f) die("no build file", grow_file);
    static char text[65536];
    int n = (int)fread(text, 1, sizeof text - 1, f);
    text[n < 0 ? 0 : n] = 0;
    fclose(f);

    parse(text);
    if (had_error) exit(1);

    if (command && !strcmp(command, "list"))  { cmd_list(); return 0; }
    if (command && !strcmp(command, "clean")) { cmd_clean(); return 0; }
    if (command && !strcmp(command, "info"))  { cmd_info(npos > 1 ? pos[1] : 0); return 0; }
    cmd_grow(pos, npos);   /* build the named targets/subdirs, or everything */
    return 0;
}
