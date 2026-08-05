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

extern const BerryServices *pod_svc;   /* stashed by the pod-libc crt0 */

#define TCC_PATH   "/sys/TCC.POD"
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
    if (strcmp(kw, "pod") && strcmp(kw, "packet")) { errline(line, "unknown block keyword", kw); return; }
    if (ntarget >= MAX_TARGET) { errline(line, "too many targets", 0); return; }
    Target *t = &targets[ntarget++];
    memset(t, 0, sizeof *t);
    t->line = line;
    t->kind = kw[0] == 'p' && kw[1] == 'a' ? 'K' : 'P';
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
    return pod_svc->spawn(TCC_PATH, cargc, (const char *const *)cargv);
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

static int build_target(Target *t) {
    if (t->built) return 0;
    return t->kind == 'K' ? build_packet(t) : build_pod(t);
}

/* ------------------------------------------------------------- commands */
static void cmd_list(void) {
    printf("targets in %s:\n", grow_file);
    for (int i = 0; i < ntarget; i++) {
        Target *t = &targets[i];
        printf("  %-7s %-12s source: %s\n", t->kind == 'K' ? "packet" : "pod", t->name, t->source);
        if (t->use[0]) printf("                       use: %s\n", t->use);
    }
}
static void cmd_clean(void) {
    for (int i = 0; i < ntarget; i++) {
        Target *t = &targets[i];
        char p[64]; out_path(t, t->kind == 'K' ? "PKT" : "POD", p, sizeof p);
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
static void cmd_build(const char *only) {
    if (pod_svc->mkdir) pod_svc->mkdir(proj_out);   /* ensure the output directory */
    int n = 0;
    /* packets first (a pod's use pulls them in on demand too), then pods */
    for (int pass = 0; pass < 2 && !had_error; pass++)
        for (int i = 0; i < ntarget && !had_error; i++) {
            Target *t = &targets[i];
            if ((pass == 0) != (t->kind == 'K')) continue;
            if (only && strcmp(t->name, only)) continue;
            if (!t->built) { if (build_target(t) == 0) n++; }
        }
    if (had_error) { printf("grow: build failed\n"); exit(1); }
    printf("built %d target%s\n", n, n == 1 ? "" : "s");
}

/* ------------------------------------------------------------------ main */
int main(int argc, char **argv) {
    const char *command = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "-v")) opt_verbose = 1;
        else if (!strcmp(a, "-n")) opt_dryrun = 1;
        else if (!strcmp(a, "-f") && i + 1 < argc) grow_file = argv[++i];
        else if (!strcmp(a, "-s") && i + 1 < argc) {
            char *eq = strchr(argv[++i], '=');
            if (eq) { *eq = 0; var_set(argv[i], eq + 1); }
        } else if (a[0] != '-' && !command) command = a;
    }

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
    cmd_build(command);   /* build all, or just `command` if it names a target */
    return 0;
}
