/*
 * ps3iso_core.c - PS3 ISO builder and unpacker, see ps3iso_core.h.
 *
 * The builder is a port of makeps3iso.c (c) 2013 Estwald/Hermes, bucanero's
 * ps3iso-utils, GPL-3.0: the sizes, the directory-sort quirk, the record
 * paddings and the header bytes are reproduced on purpose so Cobra/webMAN see
 * the same ISO they see from makeps3iso.  Dates are the caller's.
 */
#include "ps3iso_core.h"

#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#ifndef PS3ISO_EXTENT_MAX
#define PS3ISO_EXTENT_MAX 0xFFFFF800u   /* makeps3iso: one extent < 4 GB, multiple of 2048 */
#endif

#define SEC        2048u
#define CHUNK      0x40000u             /* 256 KiB streaming chunk */
#define MAX_DIRS   65535u
#define MAX_DEPTH  100
/* makeps3iso allows 222 but writes records longer than 255 bytes (corrupt) beyond these:
 * ISO file record 41+len <= 254 -> 213 bytes, Joliet file record 43+2*units <= 254 -> 105 units */
#define MAX_NAME   213
#define MAX_UNITS  105
#define PATHCAP    1024

const char *ps3iso_strerror(int code)
{
    switch (code) {
    case PS3ISO_OK:               return "ok";
    case PS3ISO_E_ARG:            return "bad argument";
    case PS3ISO_E_NOMEM:          return "out of memory";
    case PS3ISO_E_LIST:           return "directory listing failed";
    case PS3ISO_E_READ:           return "reading a file failed";
    case PS3ISO_E_SINK:           return "writing the output failed";
    case PS3ISO_E_NO_SFO:         return "PS3_GAME/PARAM.SFO missing or without TITLE/TITLE_ID";
    case PS3ISO_E_NAME_TOO_LONG:  return "name too long (max 213 bytes / 105 UTF-16 units)";
    case PS3ISO_E_TOO_MANY_DIRS:  return "too many folders";
    case PS3ISO_E_DIR_FULL:       return "too many entries in a directory";
    case PS3ISO_E_SPLIT666:       return ".666NN split files are not supported";
    case PS3ISO_E_TOO_DEEP:       return "folders nested too deep";
    case PS3ISO_E_TOO_BIG:        return "image larger than 2^32 sectors";
    case PS3ISO_E_STATE:          return "internal consistency check failed";
    case PS3ISO_E_BAD_ISO:        return "inconsistent ISO image";
    default:                      return "unknown error";
    }
}

/* ------------------------------------------------------------------ */
/* both-endian helpers (makeps3iso's set7xx)                           */
/* ------------------------------------------------------------------ */

static void set731(u8 *p, u32 n) { p[0] = (u8)n; p[1] = (u8)(n >> 8); p[2] = (u8)(n >> 16); p[3] = (u8)(n >> 24); }
static void set732(u8 *p, u32 n) { p[0] = (u8)(n >> 24); p[1] = (u8)(n >> 16); p[2] = (u8)(n >> 8); p[3] = (u8)n; }
static void set733(u8 *p, u32 n) { set731(p, n); set732(p + 4, n); }
static void set721(u8 *p, u32 n) { p[0] = (u8)n; p[1] = (u8)(n >> 8); }
static void set722(u8 *p, u32 n) { p[0] = (u8)(n >> 8); p[1] = (u8)n; }
static void set723(u8 *p, u32 n) { set721(p, n); set722(p + 2, n); }

static u32 get731(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static u32 get732(const u8 *p) { return (u32)p[3] | ((u32)p[2] << 8) | ((u32)p[1] << 16) | ((u32)p[0] << 24); }

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

static void civil_from_secs(int64_t t, int *Y, int *M, int *D, int *h, int *mi, int *s)
{
    int64_t days, rem, z, era, doe, yoe, y, doy, mp;
    if (t < 0) t = 0;
    days = t / 86400; rem = t % 86400;
    *h = (int)(rem / 3600); *mi = (int)((rem % 3600) / 60); *s = (int)(rem % 60);
    z = days + 719468;
    era = z / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *D = (int)(doy - (153 * mp + 2) / 5 + 1);
    *M = (int)(mp < 10 ? mp + 3 : mp - 9);
    *Y = (int)(y + (*M <= 2));
}

static void set_daterec(u8 *p, int64_t t)
{
    int Y, M, D, h, mi, s;
    civil_from_secs(t, &Y, &M, &D, &h, &mi, &s);
    p[0] = (u8)((Y - 1900) & 255); p[1] = (u8)(M & 15); p[2] = (u8)(D & 31);
    p[3] = (u8)h; p[4] = (u8)mi; p[5] = (u8)s; p[6] = 0;
}

static void put_dec(u8 *p, int n, int digits)
{
    while (digits-- > 0) { p[digits] = (u8)('0' + n % 10); n /= 10; }
}

static void set_longdate(u8 *p, int64_t t)   /* "YYYYMMDDhhmmss00" + GMT byte 0 */
{
    int Y, M, D, h, mi, s;
    civil_from_secs(t, &Y, &M, &D, &h, &mi, &s);
    put_dec(p, Y, 4); put_dec(p + 4, M, 2); put_dec(p + 6, D, 2);
    put_dec(p + 8, h, 2); put_dec(p + 10, mi, 2); put_dec(p + 12, s, 2);
    p[14] = '0'; p[15] = '0'; p[16] = 0;
}

/* ------------------------------------------------------------------ */
/* names                                                               */
/* ------------------------------------------------------------------ */

/* makeps3iso's UTF8_to_UTF16 (including its quirks), output as host u16; the
 * caller stores them big endian. Returns the number of units. */
static u32 utf8_to_utf16(const u8 *stb, u16 *w, u32 max)
{
    u32 cnt = 0;
    int n, m;
    u32 UTF32;
    while (*stb && cnt + 2 < max) {
        if (*stb & 128) {
            m = 1;
            if ((*stb & 0xf8) == 0xf0)      { UTF32 = (u32)(*(stb++) & 7);   m = 3; }   /* makeps3iso: & 3 */
            else if ((*stb & 0xE0) == 0xE0) { UTF32 = (u32)(*(stb++) & 0xf);  m = 2; }
            else if ((*stb & 0xE0) == 0xC0) { UTF32 = (u32)(*(stb++) & 0x1f); m = 1; }
            else { stb++; continue; }
            for (n = 0; n < m; n++) {
                if (!*stb) break;
                if ((*stb & 0xc0) != 0x80) break;
                UTF32 = (UTF32 << 6) | (u32)(*(stb++) & 63);
            }
            if ((n != m) && !*stb) break;
        } else UTF32 = (u32)*(stb++);

        if (UTF32 < 65536) w[cnt++] = (u16)UTF32;
        else {   /* makeps3iso forgets the - 0x10000 here and writes a wrong pair; ours is correct UTF-16 */
            UTF32 -= 0x10000;
            w[cnt++] = (u16)((((u16)(UTF32 >> 10)) & 0x3ff) | 0xD800);
            w[cnt++] = (u16)((((u16)UTF32) & 0x3ff) | 0xDC00);
        }
    }
    w[cnt] = 0;
    return cnt;
}

static void put_u16be(u8 *p, const u16 *w, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++) { p[2 * i] = (u8)(w[i] >> 8); p[2 * i + 1] = (u8)w[i]; }
}

static u8 upcase(u8 c) { return (c >= 'a' && c <= 'z') ? (u8)(c - 32) : c; }

static void memcapscpy(u8 *d, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) d[i] = upcase((u8)s[i]);
}

/* makeps3iso's utf8_to_ansiname: ch must be NUL padded for a few bytes */
static void utf8_to_ansiname(u8 *ch, char *ansi, int len)
{
    u8 c;
    int is_space = 1;
    char *a = ansi;

    *ansi = 0;
    while (*ch != 0 && len > 0) {
        if (((*ch & 0xF1) == 0xF0 || (*ch & 0xF0) == 0xe0) && (*(ch + 1) & 0xc0) == 0x80) {
            if (!is_space) { *ansi++ = ' '; len--; is_space = 1; }
            ch += 2 + 1 * ((*ch & 0xF1) == 0xF0);
        } else if ((*ch & 0xE0) == 0xc0 && (*(ch + 1) & 0xc0) == 0x80) {
            c = (u8)(((*ch & 3) << 6) | (*(ch + 1) & 63));
            if (c >= 0xC0 && c <= 0xC5) c = 'A';
            else if (c == 0xc7) c = 'C';
            else if (c >= 0xc8 && c <= 0xcb) c = 'E';
            else if (c >= 0xcc && c <= 0xcf) c = 'I';
            else if (c == 0xd1) c = 'N';
            else if (c >= 0xd2 && c <= 0xd6) c = 'O';
            else if (c >= 0xd9 && c <= 0xdc) c = 'U';
            else if (c == 0xdd) c = 'Y';
            else if (c >= 0xe0 && c <= 0xe5) c = 'a';
            else if (c == 0xe7) c = 'c';
            else if (c >= 0xe8 && c <= 0xeb) c = 'e';
            else if (c >= 0xec && c <= 0xef) c = 'i';
            else if (c == 0xf1) c = 'n';
            else if (c >= 0xf2 && c <= 0xf6) c = 'o';
            else if (c >= 0xf9 && c <= 0xfc) c = 'u';
            else if (c == 0xfd || c == 0xff) c = 'y';
            else if (c > 127) c = *(++ch + 1);
            if (!is_space || c != 32) {
                *ansi++ = (char)c; len--;
                is_space = (c == 32);
            }
            ch++;
        } else {
            if (*ch < 32) *ch = 32;
            if (!is_space || *ch != 32) {
                *ansi++ = (char)*ch; len--;
                is_space = (*ch == 32);
            }
        }
        ch++;
    }
    while (len > 0) { *ansi++ = 0; len--; }
    if (a[0] == 0 || a[0] == ' ') strcpy(a, "PS3");
}

static void fixtitle(char *p)
{
    while (*p) {
        if (*p & 128) *p = 0;
        else if (*p == ':' || *p == '?' || *p == '"' || *p == '<' || *p == '>' || *p == '|') *p = '_';
        else if (*p == '\\' || *p == '/') *p = '-';
        else if (((u8)*p) > 0 && ((u8)*p) < 32) { *p = 0; break; }
        p++;
    }
}

/* ------------------------------------------------------------------ */
/* builder data                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    u32     name;      /* offset in pool */
    u32     ulen;      /* UTF-16 units */
    u64     size;
    int64_t mtime;
    u32     lba;       /* first sector, files placed in the image only */
    u8      is_dir;
} ent_t;

typedef struct {       /* makeps3iso's directory_iso[] */
    u32     ldir, wdir;      /* sectors of the ISO / Joliet record area */
    u32     llba, wlba;
    int     parent;          /* 1-based index into the sorted table */
    u32     name;            /* pool offset */
    u32     ulen;
    u32     first, count;    /* its entries in ents[] */
    int64_t mtime;
} diso_t;

typedef struct { u32 ent; u32 level; } fo_t;

struct ps3iso_build {
    ps3iso_src src;
    char  *pool;  size_t pool_len, pool_cap;
    ent_t *ents;  u32 nents, cap_ents;
    diso_t *dir;  u32 ndirs, cap_dirs;
    fo_t  *fo;    u32 nfo;
    u32 lpath, wpath;
    u32 llba0, llba1, wlba0, wlba1, dllba, dwlba, dlsz, dwsz, flba2, toc;
    char title_id[64];
    char title_name[64];
    int  planned;
    char msg[200];
};

typedef struct ps3iso_build B;

static void bmsg(B *b, const char *a, const char *name)
{
    size_t n = 0, i;
    b->msg[0] = 0;
    for (i = 0; a[i] && n < sizeof(b->msg) - 1; i++) b->msg[n++] = a[i];
    if (name) {
        if (n < sizeof(b->msg) - 2) b->msg[n++] = ' ';
        for (i = 0; name[i] && n < sizeof(b->msg) - 1; i++) b->msg[n++] = name[i];
    }
    b->msg[n] = 0;
}

static int pool_add(B *b, const char *s, u32 *off)
{
    size_t n = strlen(s) + 1;
    if (b->pool_len + n > b->pool_cap) {
        size_t nc = b->pool_cap ? b->pool_cap * 2 : 4096;
        char *p;
        while (nc < b->pool_len + n) nc *= 2;
        p = (char *)realloc(b->pool, nc);
        if (!p) return PS3ISO_E_NOMEM;
        b->pool = p; b->pool_cap = nc;
    }
    memcpy(b->pool + b->pool_len, s, n);
    *off = (u32)b->pool_len;
    b->pool_len += n;
    return 0;
}

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), c = strlen(suf);
    return a >= c && !strcmp(s + a - c, suf);
}

/* name of an update folder: its files are kept out of the image (makeps3iso NOPS3_UPDATE) */
static int is_update_dir(const B *b, u32 level)
{
    return level != 0 && ends_with(b->pool + b->dir[level].name, "PS3_UPDATE");
}

typedef struct { B *b; int err; } add_ctx;

static int add_cb(void *u, const ps3iso_entry *e)
{
    add_ctx *c = (add_ctx *)u;
    B *b = c->b;
    ent_t *t;
    u16 w[1024];
    size_t lname;

    if (c->err) return c->err;
    if (!e || !e->name) { c->err = PS3ISO_E_ARG; return c->err; }
    if (e->name[0] == '.' && (e->name[1] == '.' || e->name[1] == 0)) return 0;   /* makeps3iso's test */
    lname = strlen(e->name);
    if (lname == 0 || strchr(e->name, '/')) { bmsg(b, "bad name", e->name); c->err = PS3ISO_E_ARG; return c->err; }
    if (lname > MAX_NAME) { bmsg(b, "name too long:", e->name); c->err = PS3ISO_E_NAME_TOO_LONG; return c->err; }
    if (!e->is_dir && lname >= 6 && !strncmp(e->name + lname - 6, ".666", 4)) {
        bmsg(b, "split file:", e->name); c->err = PS3ISO_E_SPLIT666; return c->err;
    }
    if (b->nents == b->cap_ents) {
        u32 nc = b->cap_ents ? b->cap_ents * 2 : 256;
        ent_t *p = (ent_t *)realloc(b->ents, (size_t)nc * sizeof(ent_t));
        if (!p) { c->err = PS3ISO_E_NOMEM; return c->err; }
        b->ents = p; b->cap_ents = nc;
    }
    t = &b->ents[b->nents];
    memset(t, 0, sizeof(*t));
    t->ulen = utf8_to_utf16((const u8 *)e->name, w, 1024);
    if (t->ulen > MAX_UNITS) { bmsg(b, "name too long:", e->name); c->err = PS3ISO_E_NAME_TOO_LONG; return c->err; }
    if ((c->err = pool_add(b, e->name, &t->name)) != 0) return c->err;
    t->size = e->is_dir ? 0 : e->size;
    t->mtime = e->mtime;
    t->is_dir = e->is_dir ? 1 : 0;
    b->nents++;
    return 0;
}

/* one step of makeps3iso's calc_entries record-area accounting */
static void dir_acc(u32 *tot, u32 *cur, u32 add)
{
    *cur += add;
    if (*cur > 2048) {
        *tot = (*tot & ~2047u) + 2048;
        *cur = add;
    } else if (*cur == 2048) {
        *cur = 0;
    }
    *tot += add;
}

static u32 even(u32 x) { return x + (x & 1); }

static u32 parts_of(u64 size)
{
    if (size == 0) return 1;
    return (u32)((size + (PS3ISO_EXTENT_MAX - 1)) / PS3ISO_EXTENT_MAX);
}

static int scan_dir(B *b, char *path, size_t plen, int parent1, int depth, u32 name, u32 ulen, int64_t mtime)
{
    u32 me, i;
    int rc;
    add_ctx ac;
    u32 ldir, wdir, cldir, cwdir;
    diso_t *d;

    if (depth > MAX_DEPTH) return PS3ISO_E_TOO_DEEP;
    if (b->ndirs >= MAX_DIRS) return PS3ISO_E_TOO_MANY_DIRS;
    if (b->ndirs == b->cap_dirs) {
        u32 nc = b->cap_dirs ? b->cap_dirs * 2 : 64;
        diso_t *p = (diso_t *)realloc(b->dir, ((size_t)nc + 1) * sizeof(diso_t));
        if (!p) return PS3ISO_E_NOMEM;
        b->dir = p; b->cap_dirs = nc;
    }
    me = b->ndirs++;
    d = &b->dir[me];
    memset(d, 0, sizeof(*d));
    d->parent = parent1; d->name = name; d->ulen = ulen; d->mtime = mtime;
    d->first = b->nents;

    ac.b = b; ac.err = 0;
    rc = b->src.list(b->src.ctx, path, add_cb, &ac);
    if (ac.err) return ac.err;
    if (rc) { bmsg(b, "list failed:", path); return PS3ISO_E_LIST; }
    d = &b->dir[me];
    d->count = b->nents - d->first;

    /* makeps3iso: "." and ".." records take 80 bytes in both trees */
    ldir = even(34 + 6); ldir = (ldir + 7) & ~7u; ldir += 34 + 6; ldir += ldir & 1;
    wdir = ldir;
    cldir = ldir; cwdir = wdir;

    for (i = 0; i < d->count; i++) {      /* files */
        const ent_t *e = &b->ents[d->first + i];
        const char *nm = b->pool + e->name;
        u32 lname = (u32)strlen(nm), parts = parts_of(e->size), n, add;
        if (e->is_dir) continue;
        for (n = 0; n < parts; n++) {
            add = 34 + lname - 1 + 8; add += add & 1;
            dir_acc(&ldir, &cldir, add);
            add = 34 - 1 + e->ulen * 2 + 4 + 6; add += add & 1;
            dir_acc(&wdir, &cwdir, add);
        }
    }
    for (i = 0; i < d->count; i++) {      /* directories */
        const ent_t *e = &b->ents[d->first + i];
        const char *nm = b->pool + e->name;
        u32 lname = (u32)strlen(nm), add;
        if (!e->is_dir) continue;
        b->lpath += 9 + lname - 1; b->lpath += b->lpath & 1;
        add = 34 + lname - 1 + 6; add += add & 1;
        dir_acc(&ldir, &cldir, add);
        b->wpath += 9 + e->ulen * 2 - 1; b->wpath += b->wpath & 1;
        add = 34 + e->ulen * 2 - 1 + 6; add += add & 1;
        dir_acc(&wdir, &cwdir, add);
    }
    d->ldir = (ldir + 2047) / 2048;
    d->wdir = (wdir + 2047) / 2048;

    for (i = 0; i < d->count; i++) {      /* recursion, in listing order */
        ent_t e;
        size_t l2;
        e = b->ents[b->dir[me].first + i];
        if (!e.is_dir) continue;
        l2 = strlen(b->pool + e.name);
        if (plen + 1 + l2 >= PATHCAP) return PS3ISO_E_TOO_DEEP;
        if (plen) { path[plen] = '/'; memcpy(path + plen + 1, b->pool + e.name, l2 + 1); }
        else memcpy(path, b->pool + e.name, l2 + 1);
        rc = scan_dir(b, path, plen ? plen + 1 + l2 : l2, me + 1, depth + 1, e.name, e.ulen, e.mtime);
        path[plen] = 0;
        if (rc) return rc;
    }
    return 0;
}

/* PARAM.SFO: makeps3iso's parse_param_sfo, bounds checked */
static int parse_sfo(const u8 *mem, size_t len, char *title_id, char *title_name)
{
    size_t str, pos;
    unsigned indx = 0;
    int ct = 0;
    if (len < 0x20) return PS3ISO_E_NO_SFO;
    str = (size_t)mem[8] + ((size_t)mem[9] << 8);
    pos = (size_t)mem[0xc] + ((size_t)mem[0xd] << 8);
    while (str < len) {
        if (mem[str] == 0) break;
        if (!strcmp((const char *)&mem[str], "TITLE")) {
            size_t k;
            for (k = 0; k < 63 && pos + k < len && mem[pos + k]; k++) title_name[k] = (char)mem[pos + k];
            ct++;
        } else if (!strcmp((const char *)&mem[str], "TITLE_ID")) {
            size_t k;
            if (pos + 4 > len) return PS3ISO_E_NO_SFO;
            memcpy(title_id, &mem[pos], 4);
            title_id[4] = '-';
            for (k = 0; k < 58 && pos + 4 + k < len && mem[pos + 4 + k]; k++) title_id[5 + k] = (char)mem[pos + 4 + k];
            ct++;
        }
        if (ct == 2) return 0;
        while (mem[str]) str++;
        str++;
        if ((size_t)indx + 0x1d >= len) break;
        pos += (size_t)mem[0x1c + indx] + ((size_t)mem[0x1d + indx] << 8);
        indx += 16;
    }
    return PS3ISO_E_NO_SFO;
}

static int load_sfo(B *b)
{
    const diso_t *root = &b->dir[0];
    u32 i, j;
    int rc;
    u8 *mem;
    for (i = 0; i < root->count; i++) {
        const ent_t *g = &b->ents[root->first + i];
        if (!g->is_dir || strcmp(b->pool + g->name, "PS3_GAME")) continue;
        for (j = 1; j < b->ndirs; j++) {     /* its directory: parent root, same name */
            const diso_t *c = &b->dir[j];
            u32 k;
            if (c->parent != 1 || c->name != g->name) continue;
            for (k = 0; k < c->count; k++) {
                const ent_t *f = &b->ents[c->first + k];
                if (f->is_dir || strcmp(b->pool + f->name, "PARAM.SFO")) continue;
                if (f->size < 0x20 || f->size > 0x40000) return PS3ISO_E_NO_SFO;
                mem = (u8 *)calloc(1, (size_t)f->size + 16);
                if (!mem) return PS3ISO_E_NOMEM;
                if (b->src.read(b->src.ctx, "PS3_GAME/PARAM.SFO", 0, mem, (u32)f->size)) {
                    free(mem);
                    bmsg(b, "cannot read", "PS3_GAME/PARAM.SFO");
                    return PS3ISO_E_READ;
                }
                memset(b->title_id, 0, sizeof(b->title_id));
                memset(b->title_name, 0, sizeof(b->title_name));
                rc = parse_sfo(mem, (size_t)f->size, b->title_id, b->title_name);
                free(mem);
                if (rc) bmsg(b, "no TITLE/TITLE_ID in", "PS3_GAME/PARAM.SFO");
                return rc;
            }
        }
    }
    bmsg(b, "missing", "PS3_GAME/PARAM.SFO");
    return PS3ISO_E_NO_SFO;
}

static int place_level(B *b, u32 level, u64 *flba)
{
    const diso_t *d = &b->dir[level];
    u32 i, n;
    int rc;
    if (!is_update_dir(b, level)) {
        for (i = 0; i < d->count; i++) {
            ent_t *e = &b->ents[d->first + i];
            u64 sectors;
            if (e->is_dir) continue;
            sectors = (e->size + 2047) / 2048;
            if (*flba + sectors > 0xFFFFFFFFull) return PS3ISO_E_TOO_BIG;
            e->lba = (u32)*flba;
            *flba += sectors;
            if (b->nfo % 256 == 0) {
                fo_t *p = (fo_t *)realloc(b->fo, ((size_t)b->nfo + 256) * sizeof(fo_t));
                if (!p) return PS3ISO_E_NOMEM;
                b->fo = p;
            }
            b->fo[b->nfo].ent = d->first + i;
            b->fo[b->nfo].level = level;
            b->nfo++;
        }
    }
    for (n = 1; n < b->ndirs; n++)
        if (b->dir[n].parent == (int)level + 1) {
            rc = place_level(b, n, flba);
            if (rc) return rc;
        }
    return 0;
}

int ps3iso_build_plan(const ps3iso_src *src, ps3iso_build **out, uint64_t *total_size)
{
    B *b;
    char path[PATHCAP];
    u32 n, m, l, lp, wp;
    int rc;
    u64 flba;

    if (!src || !src->list || !src->read || !out) return PS3ISO_E_ARG;
    *out = NULL;
    b = (B *)calloc(1, sizeof(B));
    if (!b) return PS3ISO_E_NOMEM;
    b->src = *src;
    b->lpath = 10;         /* makeps3iso: 9, padded at the first calc_entries() */
    b->wpath = 10;
    path[0] = 0;
    {   u32 z; if ((rc = pool_add(b, "", &z)) != 0) goto fail; }   /* offset 0 = "" (root's name) */

    rc = scan_dir(b, path, 0, 1, 0, 0, 0, src->root_mtime);
    if (rc) goto fail;
    rc = load_sfo(b);
    if (rc) goto fail;

    lp = (b->lpath + 2047) / 2048;
    wp = (b->wpath + 2047) / 2048;
    b->llba0 = 20;
    b->llba1 = b->llba0 + lp;
    b->wlba0 = b->llba1 + lp;
    b->wlba1 = b->wlba0 + wp;
    b->dllba = b->wlba1 + wp;
    if (b->dllba < 32) b->dllba = 32;

    /* makeps3iso's "searching..." exchange sort of the directory table by parent */
    for (n = 1; n + 1 < b->ndirs; n++)
        for (m = n + 1; m < b->ndirs; m++) {
            if (b->dir[n].parent > b->dir[m].parent) {
                b->dir[b->ndirs] = b->dir[n]; b->dir[n] = b->dir[m]; b->dir[m] = b->dir[b->ndirs];
                for (l = n; l < b->ndirs; l++) {
                    if ((int)n + 1 == b->dir[l].parent) b->dir[l].parent = (int)m + 1;
                    else if ((int)m + 1 == b->dir[l].parent) b->dir[l].parent = (int)n + 1;
                }
            }
        }

    for (n = 0; n < b->ndirs; n++) { b->dlsz += b->dir[n].ldir; b->dwsz += b->dir[n].wdir; }
    b->dwlba = b->dllba + b->dlsz;
    b->flba2 = b->dwlba + b->dwsz;
    {
        u32 lba0 = b->dllba, lba1 = b->dwlba;
        for (n = 0; n < b->ndirs; n++) {
            b->dir[n].llba = lba0; b->dir[n].wlba = lba1;
            lba0 += b->dir[n].ldir; lba1 += b->dir[n].wdir;
        }
    }

    flba = b->flba2;
    rc = place_level(b, 0, &flba);
    if (rc) goto fail;
    b->toc = (u32)flba;
    b->planned = 1;
    if (total_size) *total_size = (u64)b->toc * SEC;
    *out = b;
    return 0;
fail:
    if (!b->msg[0]) bmsg(b, ps3iso_strerror(rc), NULL);
    *out = b;               /* carries the message only; the caller frees it */
    return rc;
}

/* ------------------------------------------------------------------ */
/* emit                                                                */
/* ------------------------------------------------------------------ */

static void put_rec(u8 *p, u32 reclen, u32 extent, u32 size, int64_t t, u8 flags, u32 namelen)
{
    p[0] = (u8)reclen; p[1] = 0;
    set733(p + 2, extent);
    set733(p + 10, size);
    set_daterec(p + 18, t);
    p[25] = flags; p[26] = 0; p[27] = 0;
    set723(p + 28, 1);
    p[32] = (u8)namelen;
}

/* room for a record of `add` bytes: move to the next sector when it does not fit */
static int rec_room(u32 *off, u32 *cnt, u32 max, u32 add)
{
    u32 c = (*off & 2047) + add;
    if (c > 2048) {
        (*cnt)++;
        if (*cnt > max) return PS3ISO_E_DIR_FULL;
        *off += add - (c - 2048);
    }
    return 0;
}

typedef struct { u32 l0, l1, w0, w1; } ptpos_t;

static void fill_dirpath(B *b, u8 *buf, ptpos_t *pp)
{
    u32 n;
    u16 w[1024];
    for (n = 0; n < b->ndirs; n++) {
        const diso_t *d = &b->dir[n];
        u8 *r0 = buf + pp->l0, *r1 = buf + pp->l1, *x0 = buf + pp->w0, *x1 = buf + pp->w1;
        if (!n) {
            set721(r0, 1); set731(r0 + 2, d->llba); set721(r0 + 6, (u32)d->parent); r0[8] = 0;
            pp->l0 += 9; pp->l0 += pp->l0 & 1;
            set721(r1, 1); set732(r1 + 2, d->llba); set722(r1 + 6, (u32)d->parent); r1[8] = 0;
            pp->l1 += 9; pp->l1 += pp->l1 & 1;
            set721(x0, 1); set731(x0 + 2, d->wlba); set721(x0 + 6, (u32)d->parent); x0[8] = 0;
            pp->w0 += 9; pp->w0 += pp->w0 & 1;
            set721(x1, 1); set732(x1 + 2, d->wlba); set722(x1 + 6, (u32)d->parent); x1[8] = 0;
            pp->w1 += 9; pp->w1 += pp->w1 & 1;
            continue;
        }
        {
            const char *nm = b->pool + d->name;
            u32 ln = (u32)strlen(nm), ls;
            ls = utf8_to_utf16((const u8 *)nm, w, 1024);

            set721(r0, ln); set731(r0 + 2, d->llba); set721(r0 + 6, (u32)d->parent);
            memcapscpy(r0 + 8, nm, ln);
            pp->l0 += 9 - 1 + ln; pp->l0 += pp->l0 & 1;

            set721(r1, ln); set732(r1 + 2, d->llba); set722(r1 + 6, (u32)d->parent);
            memcapscpy(r1 + 8, nm, ln);
            pp->l1 += 9 - 1 + ln; pp->l1 += pp->l1 & 1;

            set721(x0, ls * 2); set731(x0 + 2, d->wlba); set721(x0 + 6, (u32)d->parent);
            put_u16be(x0 + 8, w, ls);
            pp->w0 += 9 - 1 + ls * 2; pp->w0 += pp->w0 & 1;

            set721(x1, ls * 2); set732(x1 + 2, d->wlba); set722(x1 + 6, (u32)d->parent);
            put_u16be(x1 + 8, w, ls);
            pp->w1 += 9 - 1 + ls * 2; pp->w1 += pp->w1 & 1;
        }
    }
}

static int fill_level(B *b, u8 *buf, u32 level, ptpos_t *pp)
{
    const diso_t *d = &b->dir[level];
    u8 *L = buf + (size_t)d->llba * SEC, *W = buf + (size_t)d->wlba * SEC;
    u32 lo = 0, wo = 0, lcnt = 1, wcnt = 1, i, n, add;
    u32 par = level ? (u32)(d->parent - 1) : 0;
    const diso_t *pd = &b->dir[par];
    int rc;
    u16 w[1024];

    if (level == 0) fill_dirpath(b, buf, pp);

    /* "." */
    put_rec(L + lo, 40, d->llba, d->ldir * SEC, d->mtime, 2, 1); L[lo + 33] = 0; lo += 40;
    put_rec(W + wo, 40, d->wlba, d->wdir * SEC, d->mtime, 2, 1); W[wo + 33] = 0; wo += 40;
    /* ".." */
    put_rec(L + lo, 40, pd->llba, pd->ldir * SEC, level ? pd->mtime : d->mtime, 2, 1); L[lo + 33] = 1; lo += 40;
    put_rec(W + wo, 40, pd->wlba, pd->wdir * SEC, level ? pd->mtime : d->mtime, 2, 1); W[wo + 33] = 1; wo += 40;

    if (!is_update_dir(b, level)) {
        for (i = 0; i < d->count; i++) {
            const ent_t *e = &b->ents[d->first + i];
            const char *nm = b->pool + e->name;
            u32 lname = (u32)strlen(nm), parts = parts_of(e->size), lba = e->lba;
            u64 left = e->size;
            u32 ls;
            if (e->is_dir) continue;
            ls = utf8_to_utf16((const u8 *)nm, w, 1024);
            for (n = 0; n < parts; n++) {
                u32 fsize;
                u8 flags = ((n + 1) != parts) ? 0x80 : 0x00;
                if (parts > 1 && (n + 1) != parts) { fsize = PS3ISO_EXTENT_MAX; left -= fsize; }
                else fsize = (u32)left;

                add = 34 - 1 + (lname + 8); add += add & 1;
                if ((rc = rec_room(&lo, &lcnt, d->ldir, add)) != 0) return rc;
                put_rec(L + lo, add, lba, fsize, e->mtime, flags, lname + 2);
                memcapscpy(L + lo + 33, nm, lname);
                L[lo + 33 + lname] = ';'; L[lo + 33 + lname + 1] = '1';
                lo += add;

                add = 34 - 1 + ls * 2 + 4 + 6; add += add & 1;
                if ((rc = rec_room(&wo, &wcnt, d->wdir, add)) != 0) return rc;
                put_rec(W + wo, add, lba, fsize, e->mtime, flags, ls * 2 + 4);
                put_u16be(W + wo + 33, w, ls);
                W[wo + 33 + ls * 2 + 0] = 0; W[wo + 33 + ls * 2 + 1] = ';';
                W[wo + 33 + ls * 2 + 2] = 0; W[wo + 33 + ls * 2 + 3] = '1';
                wo += add;

                lba += (fsize + 2047) / 2048;
            }
        }
    }

    for (n = 1; n < b->ndirs; n++) {
        const diso_t *c = &b->dir[n];
        const char *nm;
        u32 ln, ls;
        if (c->parent != (int)level + 1) continue;
        nm = b->pool + c->name;
        ln = (u32)strlen(nm);

        add = 34 - 1 + (ln + 6); add += add & 1;
        if ((rc = rec_room(&lo, &lcnt, d->ldir, add)) != 0) return rc;
        put_rec(L + lo, add, c->llba, c->ldir * SEC, c->mtime, 2, ln);
        memcapscpy(L + lo + 33, nm, ln);
        lo += add;

        ls = utf8_to_utf16((const u8 *)nm, w, 1024);
        add = 34 - 1 + ls * 2 + 6; add += add & 1;
        if ((rc = rec_room(&wo, &wcnt, d->wdir, add)) != 0) return rc;
        put_rec(W + wo, add, c->wlba, c->wdir * SEC, c->mtime, 2, ls * 2);
        put_u16be(W + wo + 33, w, ls);
        wo += add;
    }
    for (n = 1; n < b->ndirs; n++)
        if (b->dir[n].parent == (int)level + 1) {
            rc = fill_level(b, buf, n, pp);
            if (rc) return rc;
        }
    return 0;
}

static void spaces(u8 *p, size_t n) { memset(p, 32, n); }

static int build_header(B *b, u8 *s)
{
    ptpos_t pp;
    u8 *isd, *idr;
    u16 w[64];
    u32 ls, x;
    u32 i;
    u8 fecha[17];
    int rc;

    pp.l0 = b->llba0 * SEC; pp.l1 = b->llba1 * SEC; pp.w0 = b->wlba0 * SEC; pp.w1 = b->wlba1 * SEC;
    rc = fill_level(b, s, 0, &pp);
    if (rc) return rc;

    s[0x3] = 1;
    set732(&s[0x8], 0);
    set732(&s[0xC], b->toc - 1);

    strcpy((char *)&s[0x800], "PlayStation3");
    memset(&s[0x810], 32, 0x20);
    memcpy(&s[0x810], b->title_id, 10);
    x = b->src.seed ? b->src.seed : 0x2545F491u;
    for (i = 0; i < 0x1B0; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; s[0x840 + i] = (u8)(x >> 11); }
    for (i = 0; i < 0x10; i++)  { x ^= x << 13; x ^= x >> 17; x ^= x << 5; s[0x9F0 + i] = (u8)(x >> 11); }

    set_longdate(fecha, b->src.creation_time);

    /* primary volume descriptor, sector 16 */
    isd = &s[0x8000];
    isd[0] = 1; memcpy(isd + 1, "CD001", 5); isd[6] = 1;
    spaces(isd + 8, 32);
    memcpy(isd + 40, "PS3VOLUME                       ", 32);
    set733(isd + 80, b->toc);
    set723(isd + 120, 1);
    set723(isd + 124, 1);
    set723(isd + 128, 2048);
    set733(isd + 132, b->lpath);
    set731(isd + 140, b->llba0);
    set731(isd + 144, 0);
    set732(isd + 148, b->llba1);
    set732(isd + 152, 0);
    idr = isd + 156;
    idr[0] = 34; idr[1] = 0;
    set733(idr + 2, b->dir[0].llba);
    set733(idr + 10, b->dir[0].ldir * SEC);
    memcpy(idr + 18, &s[b->dir[0].llba * SEC + 18], 7);
    idr[25] = 2; idr[26] = 0; idr[27] = 0;
    set723(idr + 28, 1);
    idr[32] = 1; idr[33] = 0;
    spaces(isd + 190, 128); memcpy(isd + 190, "PS3VOLUME", 9);
    spaces(isd + 318, 128);
    spaces(isd + 446, 128);
    spaces(isd + 574, 128);
    spaces(isd + 702, 37);
    spaces(isd + 739, 37);
    spaces(isd + 776, 37);
    memcpy(isd + 813, fecha, 17);
    memcpy(isd + 830, "0000000000000000", 16);
    memcpy(isd + 847, "0000000000000000", 16);
    memcpy(isd + 864, "0000000000000000", 16);
    isd[881] = 1;

    /* Joliet supplementary descriptor, sector 17 */
    isd = &s[0x8800];
    isd[0] = 2; memcpy(isd + 1, "CD001", 5); isd[6] = 1;
    ls = utf8_to_utf16((const u8 *)"PS3VOLUME", w, 64);
    put_u16be(isd + 40, w, ls);
    set733(isd + 80, b->toc);
    set723(isd + 120, 1);
    isd[88] = 0x25; isd[89] = 0x2f; isd[90] = 0x40;
    set723(isd + 124, 1);
    set723(isd + 128, 2048);
    set733(isd + 132, b->wpath);
    set731(isd + 140, b->wlba0);
    set731(isd + 144, 0);
    set732(isd + 148, b->wlba1);
    set732(isd + 152, 0);
    idr = isd + 156;
    idr[0] = 34; idr[1] = 0;
    set733(idr + 2, b->dir[0].wlba);
    set733(idr + 10, b->dir[0].wdir * SEC);
    memcpy(idr + 18, &s[b->dir[0].wlba * SEC + 18], 7);
    idr[25] = 2; idr[26] = 0; idr[27] = 0;
    set723(idr + 28, 1);
    idr[32] = 1; idr[33] = 0;
    put_u16be(isd + 190, w, ls);
    memcpy(isd + 813, fecha, 17);
    memcpy(isd + 830, "0000000000000000", 16);
    memcpy(isd + 847, "0000000000000000", 16);
    memcpy(isd + 864, "0000000000000000", 16);
    isd[881] = 1;

    /* terminator, sector 18 */
    isd = &s[0x9000];
    isd[0] = 255; memcpy(isd + 1, "CD001", 5);
    return 0;
}

static int dir_path(const B *b, u32 level, char *buf, size_t cap)
{
    int n;
    const char *nm;
    size_t l;
    if (level == 0) { buf[0] = 0; return 0; }
    n = dir_path(b, (u32)(b->dir[level].parent - 1), buf, cap);
    if (n < 0) return -1;
    nm = b->pool + b->dir[level].name;
    l = strlen(nm);
    if ((size_t)n + 1 + l + 1 > cap) return -1;
    if (n) buf[n++] = '/';
    memcpy(buf + n, nm, l + 1);
    return n + (int)l;
}

int ps3iso_build_emit(ps3iso_build *b, ps3iso_write_fn sink, void *sink_ctx)
{
    u8 *hdr, *buf;
    size_t hbytes, pos;
    u64 emitted = 0;
    u32 i;
    int rc;
    char path[PATHCAP + 300];

    if (!b || !b->planned || !sink) return PS3ISO_E_ARG;

    hbytes = (size_t)b->flba2 * SEC;
    hdr = (u8 *)calloc(1, hbytes);
    if (!hdr) return PS3ISO_E_NOMEM;
    rc = build_header(b, hdr);          /* fails before a byte is sent when records do not fit */
    if (rc) { free(hdr); bmsg(b, "directory records do not fit", NULL); return rc; }

    for (pos = 0; pos < hbytes; ) {
        size_t n = hbytes - pos;
        if (n > CHUNK) n = CHUNK;
        if (sink(sink_ctx, hdr + pos, (u32)n)) { free(hdr); return PS3ISO_E_SINK; }
        pos += n;
    }
    emitted = hbytes;
    free(hdr);

    buf = (u8 *)malloc(CHUNK);
    if (!buf) return PS3ISO_E_NOMEM;
    for (i = 0; i < b->nfo; i++) {
        const ent_t *e = &b->ents[b->fo[i].ent];
        u64 off = 0;
        int n = dir_path(b, b->fo[i].level, path, PATHCAP);
        size_t l = strlen(b->pool + e->name);
        if (n < 0 || (size_t)n + 1 + l + 1 > sizeof(path)) { free(buf); return PS3ISO_E_TOO_DEEP; }
        if (n) path[n++] = '/';
        memcpy(path + n, b->pool + e->name, l + 1);
        while (off < e->size) {
            u32 want = (e->size - off > CHUNK) ? CHUNK : (u32)(e->size - off);
            u32 padded = (want + 2047) & ~2047u;
            if (b->src.read(b->src.ctx, path, off, buf, want)) {
                bmsg(b, "read failed:", path); free(buf); return PS3ISO_E_READ;
            }
            memset(buf + want, 0, padded - want);
            if (sink(sink_ctx, buf, padded)) { free(buf); return PS3ISO_E_SINK; }
            emitted += padded;
            off += want;
        }
    }
    free(buf);
    if (emitted != (u64)b->toc * SEC) { bmsg(b, "emitted size differs from plan", NULL); return PS3ISO_E_STATE; }
    return 0;
}

void ps3iso_build_free(ps3iso_build *b)
{
    if (!b) return;
    free(b->pool); free(b->ents); free(b->dir); free(b->fo);
    free(b);
}

const char *ps3iso_build_title_id(const ps3iso_build *b) { return b ? b->title_id : ""; }
const char *ps3iso_build_title(const ps3iso_build *b)    { return b ? b->title_name : ""; }
const char *ps3iso_build_msg(const ps3iso_build *b)      { return b ? b->msg : ""; }

void ps3iso_build_default_name(const ps3iso_build *b, char *out, size_t cap)
{
    u8 in[160];
    char ansi[64];
    size_t n;
    if (!out || !cap) return;
    out[0] = 0;
    if (!b) return;
    memset(in, 0, sizeof(in));
    memcpy(in, b->title_name, 63);
    memset(ansi, 0, sizeof(ansi));
    utf8_to_ansiname(in, ansi, 32);
    ansi[32] = 0;
    fixtitle(ansi);
    n = strlen(ansi);
    if (n + 1 + strlen(b->title_id) + 1 > cap) { out[0] = 0; return; }
    strcpy(out, ansi); strcat(out, "-"); strcat(out, b->title_id);
}

/* ------------------------------------------------------------------ */
/* unpacker                                                            */
/* ------------------------------------------------------------------ */

typedef struct { u32 extent, size; } ext_t;
typedef struct { char *name; u32 extent, size; } sub_t;

typedef struct {
    const ps3iso_rd *rd;
    const ps3iso_out *out;
    ps3iso_unpack_stats *st;
    u8 *sec;                 /* one directory sector */
    u8 *data;                /* CHUNK */
    u32 vol;                 /* volume size in sectors */
    int joliet;
    char path[PATHCAP + 8];
    u32 plen;
    u32 ndirs;
} U;

static int ubad(U *u, const char *what)
{
    size_t i, n = 0;
    const char *p = u->plen ? u->path : "/";
    char *m = u->st->msg;
    m[0] = 0;
    for (i = 0; what[i] && n < sizeof(u->st->msg) - 1; i++) m[n++] = what[i];
    if (n < sizeof(u->st->msg) - 4) { m[n++] = ' '; m[n++] = '@'; m[n++] = ' '; }
    for (i = 0; p[i] && n < sizeof(u->st->msg) - 1; i++) m[n++] = p[i];
    m[n] = 0;
    return PS3ISO_E_BAD_ISO;
}

static int rd733(const u8 *p, u32 *v)
{
    u32 a = get731(p), b = get732(p + 4);
    if (a != b) return -1;
    *v = a;
    return 0;
}

static int uread(U *u, u64 off, void *buf, u32 len)
{
    return u->rd->read(u->rd->ctx, off, buf, len) ? PS3ISO_E_READ : 0;
}

/* decode a record name into UTF-8 without the ";n" version; 0 ok */
static int decode_name(const U *u, const u8 *nm, u32 nlen, char *out, size_t cap)
{
    size_t o = 0;
    if (u->joliet) {
        u32 i;
        if (nlen & 1) return -1;
        for (i = 0; i + 1 < nlen + 0; i += 2) {
            u32 c = ((u32)nm[i] << 8) | nm[i + 1];
            if (c == ';') break;
            if (c >= 0xD800 && c < 0xDC00) {
                u32 c2;
                if (i + 3 >= nlen) return -1;
                c2 = ((u32)nm[i + 2] << 8) | nm[i + 3];
                if (c2 < 0xDC00 || c2 > 0xDFFF) return -1;
                c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                i += 2;
            } else if (c >= 0xDC00 && c <= 0xDFFF) return -1;
            if (o + 5 > cap) return -1;
            if (c < 0x80) out[o++] = (char)c;
            else if (c < 0x800) { out[o++] = (char)(0xC0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 63)); }
            else if (c < 0x10000) { out[o++] = (char)(0xE0 | (c >> 12)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
            else { out[o++] = (char)(0xF0 | (c >> 18)); out[o++] = (char)(0x80 | ((c >> 12) & 63)); out[o++] = (char)(0x80 | ((c >> 6) & 63)); out[o++] = (char)(0x80 | (c & 63)); }
        }
    } else {
        u32 i;
        for (i = 0; i < nlen; i++) {
            if (nm[i] == ';') break;
            if (o + 2 > cap) return -1;
            out[o++] = (char)nm[i];
        }
    }
    out[o] = 0;
    if (o == 0 || !strcmp(out, ".") || !strcmp(out, "..") || strchr(out, '/')) return -1;
    {   /* no control bytes, no embedded NUL */
        size_t i;
        for (i = 0; i < o; i++) if ((u8)out[i] < 32) return -1;
    }
    return 0;
}

static int stream_file(U *u, const ext_t *ex, u32 nex, u64 total)
{
    void *h = NULL;
    u32 k;
    int rc = 0, rc2;
    if (u->out->create(u->out->ctx, u->path, total, &h)) return PS3ISO_E_SINK;
    for (k = 0; k < nex && !rc; k++) {
        u64 off = (u64)ex[k].extent * SEC, left = ex[k].size;
        while (left && !rc) {
            u32 n = left > CHUNK ? CHUNK : (u32)left;
            rc = uread(u, off, u->data, n);
            if (rc) { ubad(u, "cannot read file data"); break; }
            if (u->out->write(u->out->ctx, h, u->data, n)) { rc = PS3ISO_E_SINK; break; }
            off += n; left -= n;
        }
    }
    rc2 = u->out->close(u->out->ctx, h);
    if (!rc && rc2) rc = PS3ISO_E_SINK;
    if (!rc) { u->st->files++; u->st->bytes += total; }
    return rc;
}

static int walk_dir(U *u, u32 extent, u32 size, int depth)
{
    u32 s, nsec = size / SEC;
    int nrec = 0, rc = 0;
    sub_t *subs = NULL;
    u32 nsubs = 0, capsubs = 0, i;
    ext_t *chain = NULL;
    u32 nchain = 0, capchain = 0;
    char chain_name[512];
    char name[512];
    u32 save_plen = u->plen;

    if (depth > MAX_DEPTH) return ubad(u, "folders nested too deep");
    if (size == 0 || (size % SEC) || (u64)extent + nsec > u->vol) return ubad(u, "bad directory extent");
    chain_name[0] = 0;

    for (s = 0; s < nsec && !rc; s++) {
        u32 off = 0;
        rc = uread(u, ((u64)extent + s) * SEC, u->sec, SEC);
        if (rc) { ubad(u, "cannot read directory"); break; }
        while (off < SEC) {
            const u8 *r = &u->sec[off];
            u32 len = r[0], nlen, ext, sz;
            u8 flags;
            if (len == 0) break;
            if (len < 34 || off + len > SEC) { rc = ubad(u, "bad record length"); break; }
            nlen = r[32];
            if (33 + nlen > len) { rc = ubad(u, "bad name length"); break; }
            if (rd733(r + 2, &ext) || rd733(r + 10, &sz)) { rc = ubad(u, "extent/size fields disagree (both-endian)"); break; }
            flags = r[25];
            if (r[26] || r[27] || r[1]) { rc = ubad(u, "interleaved or extended-attribute record"); break; }
            if (nrec == 0) {
                if (nlen != 1 || r[33] != 0 || !(flags & 2) || ext != extent) { rc = ubad(u, "first record is not '.'"); break; }
            } else if (nrec == 1) {
                if (nlen != 1 || r[33] != 1 || !(flags & 2)) { rc = ubad(u, "second record is not '..'"); break; }
            } else {
                if (flags & 2) {
                    if (flags & 0x80) { rc = ubad(u, "multi-extent directory"); break; }
                    if (decode_name(u, r + 33, nlen, name, sizeof(name))) { rc = ubad(u, "bad folder name"); break; }
                    if (nchain) { rc = ubad(u, "folder inside a multi-extent file"); break; }
                    if (u->ndirs >= MAX_DIRS) { rc = ubad(u, "too many folders"); break; }
                    if (nsubs == capsubs) {
                        u32 nc = capsubs ? capsubs * 2 : 8;
                        sub_t *p = (sub_t *)realloc(subs, nc * sizeof(sub_t));
                        if (!p) { rc = PS3ISO_E_NOMEM; break; }
                        subs = p; capsubs = nc;
                    }
                    subs[nsubs].name = (char *)malloc(strlen(name) + 1);
                    if (!subs[nsubs].name) { rc = PS3ISO_E_NOMEM; break; }
                    strcpy(subs[nsubs].name, name);
                    subs[nsubs].extent = ext; subs[nsubs].size = sz;
                    nsubs++;
                    u->ndirs++;
                    {   /* create it now so listing order is kept */
                        size_t l = strlen(name);
                        if (u->plen + 1 + l >= PATHCAP) { rc = ubad(u, "path too long"); break; }
                        if (u->plen) u->path[u->plen] = '/';
                        memcpy(u->path + u->plen + (u->plen ? 1 : 0), name, l + 1);
                        if (u->out->mkdir(u->out->ctx, u->path)) rc = PS3ISO_E_SINK;
                        else u->st->dirs++;
                        u->path[u->plen] = 0;
                    }
                    if (rc) break;
                } else {
                    if (decode_name(u, r + 33, nlen, name, sizeof(name))) { rc = ubad(u, "bad file name"); break; }
                    if (nchain && strcmp(name, chain_name)) { rc = ubad(u, "multi-extent chain interrupted"); break; }
                    if (!nchain) strcpy(chain_name, name);
                    if (sz && ((u64)ext + (sz + 2047) / 2048 > u->vol)) { rc = ubad(u, "file data beyond the volume"); break; }
                    if (ext > u->vol) { rc = ubad(u, "file extent beyond the volume"); break; }
                    if (nchain == capchain) {
                        u32 nc = capchain ? capchain * 2 : 4;
                        ext_t *p = (ext_t *)realloc(chain, nc * sizeof(ext_t));
                        if (!p) { rc = PS3ISO_E_NOMEM; break; }
                        chain = p; capchain = nc;
                    }
                    chain[nchain].extent = ext; chain[nchain].size = sz; nchain++;
                    if (!(flags & 0x80)) {
                        u64 total = 0;
                        size_t l = strlen(name);
                        for (i = 0; i < nchain; i++) total += chain[i].size;
                        if (u->plen + 1 + l >= PATHCAP) { rc = ubad(u, "path too long"); break; }
                        if (u->plen) u->path[u->plen] = '/';
                        memcpy(u->path + u->plen + (u->plen ? 1 : 0), name, l + 1);
                        rc = stream_file(u, chain, nchain, total);
                        u->path[u->plen] = 0;
                        nchain = 0; chain_name[0] = 0;
                        if (rc) break;
                    }
                }
            }
            nrec++;
            off += len;
        }
    }
    if (!rc && nrec < 2) rc = ubad(u, "directory without '.' record");
    if (!rc && nchain) rc = ubad(u, "multi-extent file never ends");

    for (i = 0; i < nsubs && !rc; i++) {
        size_t l = strlen(subs[i].name);
        if (u->plen + 1 + l >= PATHCAP) { rc = ubad(u, "path too long"); break; }
        if (u->plen) u->path[u->plen] = '/';
        memcpy(u->path + u->plen + (u->plen ? 1 : 0), subs[i].name, l + 1);
        u->plen = u->plen + (u->plen ? 1 : 0) + (u32)l;
        rc = walk_dir(u, subs[i].extent, subs[i].size, depth + 1);
        u->plen = save_plen;
        u->path[u->plen] = 0;
    }
    for (i = 0; i < nsubs; i++) free(subs[i].name);
    free(subs);
    free(chain);
    return rc;
}

int ps3iso_unpack(const ps3iso_rd *rd, const ps3iso_out *out, ps3iso_unpack_stats *st)
{
    U u;
    u8 *d;
    u32 sector, vol = 0, rext = 0, rsize = 0;
    int have_pvd = 0, have_svd = 0, rc;
    u32 pvd_ext = 0, pvd_size = 0, svd_ext = 0, svd_size = 0, svd_vol = 0;

    if (!rd || !rd->read || !out || !out->mkdir || !out->create || !out->write || !out->close || !st) return PS3ISO_E_ARG;
    memset(st, 0, sizeof(*st));
    memset(&u, 0, sizeof(u));
    u.rd = rd; u.out = out; u.st = st;
    u.sec = (u8 *)malloc(SEC);
    u.data = (u8 *)malloc(CHUNK);
    d = (u8 *)malloc(SEC);
    if (!u.sec || !u.data || !d) { free(u.sec); free(u.data); free(d); return PS3ISO_E_NOMEM; }

    /* PS3 header (optional) */
    if (!rd->read(rd->ctx, 0x800, d, 64)) {
        if (!memcmp(d, "PlayStation3", 12)) {
            int k;
            st->ps3_header = 1;
            for (k = 0; k < 15 && d[0x10 + k] != ' ' && d[0x10 + k]; k++) st->title_id[k] = (char)d[0x10 + k];
            st->title_id[k] = 0;
        }
    }

    for (sector = 16; sector < 16 + 32; sector++) {
        u32 v, e, s2;
        if (uread(&u, (u64)sector * SEC, d, SEC)) { rc = ubad(&u, "cannot read volume descriptors"); goto done; }
        if (memcmp(d + 1, "CD001", 5)) { rc = ubad(&u, "no CD001 signature"); goto done; }
        if (d[0] == 255) break;
        if (d[0] != 1 && d[0] != 2) continue;
        if (d[6] != 1) { rc = ubad(&u, "bad descriptor version"); goto done; }
        if (rd733(d + 80, &v) || rd733(d + 132, &s2)) { rc = ubad(&u, "volume/path-table size fields disagree"); goto done; }
        if (!(d[128] == 0 && d[129] == 8 && d[130] == 8 && d[131] == 0)) { rc = ubad(&u, "logical block size is not 2048"); goto done; }
        if (d[156] != 34 || !(d[156 + 25] & 2) || rd733(d + 156 + 2, &e) || rd733(d + 156 + 10, &s2)) { rc = ubad(&u, "bad root record"); goto done; }
        if (d[0] == 1 && !have_pvd) { have_pvd = 1; pvd_ext = e; pvd_size = s2; vol = v; }
        else if (d[0] == 2 && !have_svd && d[88] == 0x25 && d[89] == 0x2f && (d[90] == 0x40 || d[90] == 0x43 || d[90] == 0x45)) {
            have_svd = 1; svd_ext = e; svd_size = s2; svd_vol = v;
        }
    }
    if (!have_pvd) { rc = ubad(&u, "no primary volume descriptor"); goto done; }
    if (have_svd && svd_vol != vol) { rc = ubad(&u, "primary and Joliet volume sizes differ"); goto done; }
    u.vol = vol;
    if (rd->size_hint && (u64)vol * SEC > rd->size_hint) { rc = ubad(&u, "volume is larger than the file"); goto done; }
    if (have_svd) { u.joliet = 1; rext = svd_ext; rsize = svd_size; }
    else { rext = pvd_ext; rsize = pvd_size; }
    st->joliet = u.joliet;

    u.plen = 0; u.path[0] = 0;
    rc = walk_dir(&u, rext, rsize, 0);
done:
    free(u.sec); free(u.data); free(d);
    return rc;
}
