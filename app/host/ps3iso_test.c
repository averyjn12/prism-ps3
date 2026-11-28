/*
 * ps3iso_test.c - host-side judges and tools for ps3iso_core (Linux, gcc).
 * Driven by ps3iso_test.py; the judges below are FROZEN: planted bugs must
 * make them fail without editing this file.
 *
 *   judge-build  <srcdir> <ref.iso> [--first-fail]
 *        build srcdir with ps3iso_core, stream the bytes against ref.iso, mask only
 *        the ranges listed in MASKS, any other difference is a failure. Also checks
 *        plan size == emitted bytes == size of ref.iso.
 *   build        <srcdir> <out.iso>          write an ISO to a file (plan size checked)
 *   unpack       <iso> <outdir>              unpack to a folder (outdir must exist)
 *   big                                      4.5 GB virtual file: multi-extent, no disk use
 *   refuse                                   error codes of the refusal paths; two builds alive at once
 *
 * Verdict lines: "VERDICT: PASS" / "VERDICT: FAIL ..." ; a crash prints no verdict.
 */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "ps3iso_core.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef unsigned long long ull;

/* ------------------------------------------------------------------ */
/* filesystem source for the builder: readdir order, like makeps3iso   */
/* ------------------------------------------------------------------ */

typedef struct {
    char base[1024];
    FILE *fp;
    char fpath[1100];
} fs_ctx;

static int64_t dir_secs(time_t m)        /* makeps3iso: localtime() */
{
    struct tm tm;
    localtime_r(&m, &tm);
    return (int64_t)timegm(&tm);
}

static int64_t file_secs(time_t m)       /* makeps3iso: localtime(mktime(gmtime())), gmtime leaves tm_isdst = 0 so DST months show +1 h */
{
    struct tm tm, tm2;
    time_t t;
    gmtime_r(&m, &tm);
    t = mktime(&tm);
    localtime_r(&t, &tm2);
    return (int64_t)timegm(&tm2);
}

static int fs_list(void *ctx, const char *dir, ps3iso_add_fn add, void *u)
{
    fs_ctx *c = (fs_ctx *)ctx;
    char p[2200];
    DIR *d;
    struct dirent *de;
    snprintf(p, sizeof p, "%s%s%s", c->base, *dir ? "/" : "", dir);
    d = opendir(p);
    if (!d) return -1;
    while ((de = readdir(d)) != NULL) {
        char q[2400];
        struct stat s;
        ps3iso_entry e;
        int rc;
        snprintf(q, sizeof q, "%s/%s", p, de->d_name);
        if (stat(q, &s) < 0) { closedir(d); return -1; }
        e.name = de->d_name;
        e.is_dir = S_ISDIR(s.st_mode) ? 1 : 0;
        e.size = (uint64_t)s.st_size;
        e.mtime = e.is_dir ? dir_secs(s.st_mtime) : file_secs(s.st_mtime);
        rc = add(u, &e);
        if (rc) { closedir(d); return rc; }
    }
    closedir(d);
    return 0;
}

static int fs_read(void *ctx, const char *path, uint64_t off, void *buf, uint32_t len)
{
    fs_ctx *c = (fs_ctx *)ctx;
    if (!c->fp || strcmp(c->fpath, path)) {
        char p[2200];
        if (c->fp) fclose(c->fp);
        snprintf(p, sizeof p, "%s/%s", c->base, path);
        c->fp = fopen(p, "rb");
        snprintf(c->fpath, sizeof c->fpath, "%s", path);
        if (!c->fp) return -1;
    }
    if (fseeko(c->fp, (off_t)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, c->fp) == len ? 0 : -1;
}

static int fs_src(ps3iso_src *src, fs_ctx *c, const char *base)
{
    struct stat s;
    memset(c, 0, sizeof *c);
    snprintf(c->base, sizeof c->base, "%s", base);
    if (stat(base, &s) < 0 || !S_ISDIR(s.st_mode)) return -1;
    memset(src, 0, sizeof *src);
    src->ctx = c;
    src->list = fs_list;
    src->read = fs_read;
    src->root_mtime = dir_secs(s.st_mtime);
    src->creation_time = (int64_t)time(NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/* judge-build                                                         */
/* ------------------------------------------------------------------ */

typedef struct { ull off, len; const char *reason; } mask_t;

/* The ONLY bytes allowed to differ from makeps3iso's own output. */
static const mask_t MASKS[] = {
    { 0x840,  0x1B0, "sector 1: makeps3iso get_rand() bytes at 0x840 (ours: deterministic xorshift)" },
    { 0x9F0,  0x010, "sector 1: makeps3iso get_rand() bytes at 0x9F0 (ours: deterministic xorshift)" },
    { 0x832D, 0x010, "PVD creation date 'YYYYMMDDhhmmss00' = time of the build (reference built 17:59 today)" },
    { 0x8B2D, 0x010, "SVD (Joliet) creation date, same reason" },
};
#define NMASK (sizeof MASKS / sizeof MASKS[0])

typedef struct {
    FILE *ref;
    ull pos, ref_size;
    ull compared, masked, masked_diff, bad;
    ull maskdiff[NMASK];
    ull first_bad[16];
    int nbad_listed, first_fail;
    unsigned char *rbuf;
} jb_t;

static int jb_sink(void *ctx, const void *buf, uint32_t len)
{
    jb_t *j = (jb_t *)ctx;
    const unsigned char *a = (const unsigned char *)buf;
    uint32_t i;
    size_t k;
    if (j->pos + len > j->ref_size) {
        j->bad++;
        printf("  emitted more than the reference holds at 0x%llx\n", j->pos);
        return -1;
    }
    if (fread(j->rbuf, 1, len, j->ref) != len) { printf("  cannot read reference\n"); return -1; }
    if (!memcmp(a, j->rbuf, len)) {
        /* bytes in masks that happen to be equal still count as masked */
        for (k = 0; k < NMASK; k++) {
            ull lo = MASKS[k].off, hi = lo + MASKS[k].len;
            ull s = j->pos > lo ? j->pos : lo, e = (j->pos + len) < hi ? (j->pos + len) : hi;
            if (e > s) j->masked += e - s;
        }
        j->compared += len;
        j->pos += len;
        return 0;
    }
    for (i = 0; i < len; i++) {
        ull o = j->pos + i;
        int m = -1;
        for (k = 0; k < NMASK; k++)
            if (o >= MASKS[k].off && o < MASKS[k].off + MASKS[k].len) { m = (int)k; break; }
        if (m >= 0) {
            j->masked++;
            if (a[i] != j->rbuf[i]) { j->masked_diff++; j->maskdiff[m]++; }
        } else if (a[i] != j->rbuf[i]) {
            j->bad++;
            if (j->nbad_listed < 16) j->first_bad[j->nbad_listed++] = o;
        }
    }
    j->compared += len;
    j->pos += len;
    if (j->bad && j->first_fail) return -1;
    return 0;
}

static int cmd_judge_build(const char *srcdir, const char *refiso, int first_fail)
{
    fs_ctx c;
    ps3iso_src src;
    ps3iso_build *b = NULL;
    jb_t j;
    uint64_t total = 0;
    int rc, ok = 1;
    struct stat s;
    size_t k;
    int i;

    memset(&j, 0, sizeof j);
    j.first_fail = first_fail;
    if (fs_src(&src, &c, srcdir)) { printf("VERDICT: FAIL cannot open source folder\n"); return 1; }
    if (stat(refiso, &s) < 0) { printf("VERDICT: FAIL cannot stat reference\n"); return 1; }
    j.ref_size = (ull)s.st_size;
    j.ref = fopen(refiso, "rb");
    j.rbuf = (unsigned char *)malloc(0x40000);
    if (!j.ref || !j.rbuf) { printf("VERDICT: FAIL cannot open reference\n"); return 1; }
    setvbuf(j.ref, NULL, _IOFBF, 1 << 20);

    rc = ps3iso_build_plan(&src, &b, &total);
    if (rc) {
        printf("plan failed: %d %s (%s)\n", rc, ps3iso_strerror(rc), b ? ps3iso_build_msg(b) : "");
        ps3iso_build_free(b);
        printf("VERDICT: FAIL plan error\n");
        return 1;
    }
    printf("plan: %llu bytes (%llu sectors), title id %s\n", (ull)total, (ull)(total / 2048), ps3iso_build_title_id(b));
    rc = ps3iso_build_emit(b, jb_sink, &j);
    if (rc && !(first_fail && j.bad)) {
        printf("emit failed: %d %s (%s)\n", rc, ps3iso_strerror(rc), ps3iso_build_msg(b));
        ok = 0;
    }
    printf("compared=%llu masked=%llu masked_bytes_that_differ=%llu unmasked_diffs=%llu\n",
           j.compared, j.masked, j.masked_diff, j.bad);
    for (k = 0; k < NMASK; k++)
        printf("  mask 0x%llx +0x%llx differs in %llu bytes : %s\n", MASKS[k].off, MASKS[k].len, j.maskdiff[k], MASKS[k].reason);
    for (i = 0; i < j.nbad_listed; i++) printf("  first unmasked difference #%d at 0x%llx\n", i, j.first_bad[i]);
    printf("plan_size=%llu emitted=%llu reference=%llu\n", (ull)total, j.pos, j.ref_size);
    if (j.bad) ok = 0;
    if ((ull)total != j.ref_size) { printf("  plan size != reference size\n"); ok = 0; }
    if (!first_fail || !j.bad) {
        if (j.pos != (ull)total) { printf("  emitted bytes != plan size\n"); ok = 0; }
    }
    ps3iso_build_free(b);
    fclose(j.ref);
    free(j.rbuf);
    if (c.fp) fclose(c.fp);
    printf("VERDICT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* build to a file                                                     */
/* ------------------------------------------------------------------ */

typedef struct { FILE *f; ull n; } fsink_t;

static int file_sink(void *ctx, const void *buf, uint32_t len)
{
    fsink_t *s = (fsink_t *)ctx;
    if (fwrite(buf, 1, len, s->f) != len) return -1;
    s->n += len;
    return 0;
}

static int cmd_build(const char *srcdir, const char *out, int64_t creation)
{
    fs_ctx c;
    ps3iso_src src;
    ps3iso_build *b = NULL;
    fsink_t s;
    uint64_t total = 0;
    int rc;
    if (fs_src(&src, &c, srcdir)) { printf("VERDICT: FAIL cannot open source folder\n"); return 1; }
    if (creation) src.creation_time = creation;
    s.f = fopen(out, "wb"); s.n = 0;
    if (!s.f) { printf("VERDICT: FAIL cannot create output\n"); return 1; }
    rc = ps3iso_build_plan(&src, &b, &total);
    if (rc) { printf("plan failed: %d %s (%s)\n", rc, ps3iso_strerror(rc), b ? ps3iso_build_msg(b) : ""); ps3iso_build_free(b); fclose(s.f); printf("VERDICT: FAIL\n"); return 1; }
    rc = ps3iso_build_emit(b, file_sink, &s);
    if (rc) { printf("emit failed: %d %s (%s)\n", rc, ps3iso_strerror(rc), ps3iso_build_msg(b)); }
    fclose(s.f);
    printf("built %llu bytes, plan %llu\n", s.n, (ull)total);
    {
        char nm[128];
        ps3iso_build_default_name(b, nm, sizeof nm);
        printf("default name: %s\n", nm);
    }
    ps3iso_build_free(b);
    if (c.fp) fclose(c.fp);
    if (rc || s.n != (ull)total) { printf("VERDICT: FAIL\n"); return 1; }
    printf("VERDICT: PASS\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* unpack to a folder                                                  */
/* ------------------------------------------------------------------ */

typedef struct { char base[1024]; FILE *iso; } up_ctx;
typedef struct { FILE *f; ull expect, got; } up_file;

static int iso_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    up_ctx *u = (up_ctx *)ctx;
    if (fseeko(u->iso, (off_t)off, SEEK_SET)) return -1;
    return fread(buf, 1, len, u->iso) == len ? 0 : -1;
}

static int up_mkdir(void *ctx, const char *rel)
{
    up_ctx *u = (up_ctx *)ctx;
    char p[2300];
    snprintf(p, sizeof p, "%s/%s", u->base, rel);
    return mkdir(p, 0777) ? -1 : 0;
}

static int up_create(void *ctx, const char *rel, uint64_t size, void **h)
{
    up_ctx *u = (up_ctx *)ctx;
    char p[2300];
    up_file *f = (up_file *)calloc(1, sizeof *f);
    snprintf(p, sizeof p, "%s/%s", u->base, rel);
    f->f = fopen(p, "wbx");
    if (!f->f) { free(f); return -1; }
    f->expect = size;
    *h = f;
    return 0;
}

static int up_write(void *ctx, void *h, const void *buf, uint32_t len)
{
    up_file *f = (up_file *)h;
    (void)ctx;
    f->got += len;
    return fwrite(buf, 1, len, f->f) == len ? 0 : -1;
}

static int up_close(void *ctx, void *h)
{
    up_file *f = (up_file *)h;
    int bad = (f->got != f->expect);
    (void)ctx;
    if (fclose(f->f)) bad = 1;
    free(f);
    return bad ? -1 : 0;
}

static int cmd_unpack(const char *iso, const char *outdir)
{
    up_ctx c;
    ps3iso_rd rd;
    ps3iso_out out;
    ps3iso_unpack_stats st;
    struct stat s;
    int rc;
    memset(&c, 0, sizeof c);
    snprintf(c.base, sizeof c.base, "%s", outdir);
    c.iso = fopen(iso, "rb");
    if (!c.iso || stat(iso, &s) < 0) { printf("VERDICT: FAIL cannot open iso\n"); return 1; }
    setvbuf(c.iso, NULL, _IOFBF, 1 << 20);
    rd.ctx = &c; rd.read = iso_read; rd.size_hint = (uint64_t)s.st_size;
    out.ctx = &c; out.mkdir = up_mkdir; out.create = up_create; out.write = up_write; out.close = up_close;
    rc = ps3iso_unpack(&rd, &out, &st);
    printf("unpack rc=%d (%s) dirs=%u files=%u bytes=%llu joliet=%d ps3_header=%d title=%s\n",
           rc, ps3iso_strerror(rc), st.dirs, st.files, (ull)st.bytes, st.joliet, st.ps3_header, st.title_id);
    if (rc) printf("  msg: %s\n", st.msg);
    fclose(c.iso);
    printf("VERDICT: %s\n", rc ? "FAIL" : "PASS");
    return rc ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* big: a 4.5 GB virtual file through the default multi-extent path     */
/* ------------------------------------------------------------------ */

#define BIGSIZE 4831838208ull   /* 4.5 GiB */
static unsigned char pat(ull i) { return (unsigned char)((i ^ (i >> 8) ^ (i >> 19) ^ (i >> 31)) * 131u + 7u); }

#define SFOLEN 0x58
static unsigned char SFO[SFOLEN];
static void init_sfo(void)
{
    memset(SFO, 0, sizeof SFO);
    memcpy(SFO, "\0PSF\1\1\0\0", 8);
    SFO[8] = 0x34; SFO[0xc] = 0x44; SFO[0x10] = 2;
    SFO[0x14 + 0] = 0;  SFO[0x14 + 2] = 4; SFO[0x14 + 3] = 2; SFO[0x14 + 4] = 2;  SFO[0x14 + 8] = 4;  SFO[0x14 + 12] = 0;
    SFO[0x24 + 0] = 6;  SFO[0x24 + 2] = 4; SFO[0x24 + 3] = 2; SFO[0x24 + 4] = 10; SFO[0x24 + 8] = 16; SFO[0x24 + 12] = 4;
    memcpy(SFO + 0x34, "TITLE\0TITLE_ID\0", 15);
    memcpy(SFO + 0x44, "T", 1);
    memcpy(SFO + 0x48, "BLUS30386", 9);
}

typedef struct { int dummy; } big_ctx;
static int big_list(void *ctx, const char *dir, ps3iso_add_fn add, void *u)
{
    ps3iso_entry e;
    int rc;
    (void)ctx;
    memset(&e, 0, sizeof e);
    e.mtime = 1700000000;
    if (!*dir) {
        e.name = "BIG.BIN"; e.size = BIGSIZE;           rc = add(u, &e); if (rc) return rc;
        e.name = "PS3_GAME"; e.is_dir = 1; e.size = 0;  return add(u, &e);
    }
    if (!strcmp(dir, "PS3_GAME")) { e.name = "PARAM.SFO"; e.size = SFOLEN; return add(u, &e); }
    return 0;
}
static int big_read(void *ctx, const char *path, uint64_t off, void *buf, uint32_t len)
{
    unsigned char *p = (unsigned char *)buf;
    uint32_t i;
    (void)ctx;
    if (!strcmp(path, "BIG.BIN")) { for (i = 0; i < len; i++) p[i] = pat(off + i); return 0; }
    if (!strcmp(path, "PS3_GAME/PARAM.SFO")) {
        for (i = 0; i < len; i++) { uint64_t o = off + i; p[i] = o < sizeof SFO ? SFO[o] : 0; }
        return 0;
    }
    return -1;
}

typedef struct { unsigned char *hdr; ull hdrlen, pos, total, bad; } bsink_t;
static int big_sink(void *ctx, const void *buf, uint32_t len)
{
    bsink_t *s = (bsink_t *)ctx;
    const unsigned char *a = (const unsigned char *)buf;
    uint32_t i;
    for (i = 0; i < len; i++) {
        ull o = s->pos + i;
        if (o < s->hdrlen) s->hdr[o] = a[i];
        else {
            ull d = o - s->hdrlen;
            if (d < BIGSIZE) { if (a[i] != pat(d)) s->bad++; }
            else if (d < ((BIGSIZE + 2047) / 2048) * 2048) { if (a[i]) s->bad++; }
        }
    }
    s->pos += len;
    return 0;
}

typedef struct { bsink_t *s; ull big_got, sfo_got, bad; int cur; } bu_t;
static int bu_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
    bu_t *u = (bu_t *)ctx;
    unsigned char *p = (unsigned char *)buf;
    uint32_t i;
    ull bigend = u->s->hdrlen + ((BIGSIZE + 2047) / 2048) * 2048;
    for (i = 0; i < len; i++) {
        ull o = off + i;
        if (o < u->s->hdrlen) p[i] = u->s->hdr[o];
        else if (o < u->s->hdrlen + BIGSIZE) p[i] = pat(o - u->s->hdrlen);
        else if (o < bigend) p[i] = 0;
        else { ull d = o - bigend; p[i] = d < sizeof SFO ? SFO[d] : 0; }
    }
    return 0;
}
static int bu_mkdir(void *ctx, const char *rel) { (void)ctx; (void)rel; return 0; }
static int bu_create(void *ctx, const char *rel, uint64_t size, void **h)
{
    bu_t *u = (bu_t *)ctx;
    if (!strcmp(rel, "BIG.BIN")) { if (size != BIGSIZE) u->bad++; u->cur = 1; }
    else if (!strcmp(rel, "PS3_GAME/PARAM.SFO")) { if (size != SFOLEN) u->bad++; u->cur = 2; }
    else u->bad++;
    *h = u;
    return 0;
}
static int bu_write(void *ctx, void *h, const void *buf, uint32_t len)
{
    bu_t *u = (bu_t *)ctx;
    const unsigned char *a = (const unsigned char *)buf;
    uint32_t i;
    (void)h;
    if (u->cur == 1) { for (i = 0; i < len; i++) if (a[i] != pat(u->big_got + i)) u->bad++; u->big_got += len; }
    else { for (i = 0; i < len; i++) { ull o = u->sfo_got + i; if (a[i] != (o < sizeof SFO ? SFO[o] : 0)) u->bad++; } u->sfo_got += len; }
    return 0;
}
static int bu_close(void *ctx, void *h) { (void)ctx; (void)h; return 0; }

static int cmd_big(void)
{
    ps3iso_src src;
    ps3iso_build *b = NULL;
    uint64_t total = 0;
    bsink_t s;
    bu_t u;
    ps3iso_rd rd;
    ps3iso_out out;
    ps3iso_unpack_stats st;
    int rc, ok = 1;
    ull bigsec = (BIGSIZE + 2047) / 2048;

    init_sfo();

    memset(&src, 0, sizeof src);
    src.list = big_list; src.read = big_read; src.root_mtime = 1700000000; src.creation_time = 1700000000;
    rc = ps3iso_build_plan(&src, &b, &total);
    if (rc) { printf("plan failed %d\n", rc); printf("VERDICT: FAIL\n"); return 1; }
    memset(&s, 0, sizeof s);
    s.hdrlen = (ull)total - bigsec * 2048 - 2048;
    s.hdr = (unsigned char *)calloc(1, (size_t)s.hdrlen);
    rc = ps3iso_build_emit(b, big_sink, &s);
    printf("big: plan=%llu emitted=%llu hdr=%llu data_mismatch=%llu rc=%d\n", (ull)total, s.pos, s.hdrlen, s.bad, rc);
    if (rc || s.pos != (ull)total || s.bad) ok = 0;
    /* the header must say: two extents, first 0xFFFFF800 with flag 0x80 */
    memset(&u, 0, sizeof u); u.s = &s;
    rd.ctx = &u; rd.read = bu_read; rd.size_hint = total;
    out.ctx = &u; out.mkdir = bu_mkdir; out.create = bu_create; out.write = bu_write; out.close = bu_close;
    rc = ps3iso_unpack(&rd, &out, &st);
    printf("big unpack: rc=%d files=%u dirs=%u bytes=%llu big_got=%llu sfo_got=%llu bad=%llu %s\n", rc, st.files, st.dirs, (ull)st.bytes, u.big_got, u.sfo_got, u.bad, st.msg);
    if (rc || st.files != 2 || st.dirs != 1 || u.big_got != BIGSIZE || u.sfo_got != SFOLEN || u.bad) ok = 0;
    {   /* count the 0x80-flag records in the ISO tree: expect exactly 1 */
        ull o, n80 = 0;
        for (o = 0; o + 34 < s.hdrlen; o++) { /* cheap scan: record = len,0,extent(8),size(8) ... flags at +25 */
            const unsigned char *r = s.hdr + o;
            if (r[0] >= 34 && r[0] < 100 && r[1] == 0 && r[25] == 0x80 && r[10] == 0x00 && r[11] == 0xF8 && r[12] == 0xFF && r[13] == 0xFF) n80++;
        }
        printf("big: 0x80-flag extent records (ISO+Joliet) = %llu (expect 2)\n", n80);
        if (n80 != 2) ok = 0;
    }
    ps3iso_build_free(b);
    free(s.hdr);
    printf("VERDICT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* refusal paths, error codes, two builds alive in one process          */
/* ------------------------------------------------------------------ */

typedef struct { int mode; } rf_ctx;

static int rf_list(void *ctx, const char *dir, ps3iso_add_fn add, void *u)
{
    rf_ctx *c = (rf_ctx *)ctx;
    ps3iso_entry e;
    char longname[128];
    memset(&e, 0, sizeof e);
    e.mtime = 1700000000;
    if (!*dir) {
        if (c->mode != 2) { e.name = "PS3_GAME"; e.is_dir = 1; if (add(u, &e)) return 1; e.is_dir = 0; }
        if (c->mode == 0) { memset(longname, 'a', 110); longname[110] = 0; e.name = longname; e.size = 10; return add(u, &e); }
        if (c->mode == 1) { e.name = "A.66601"; e.size = 10; return add(u, &e); }
        if (c->mode == 6) { e.name = "BAD.BIN"; e.size = 5000; return add(u, &e); }
        if (c->mode >= 8) { e.name = "A.BIN"; e.size = 3000; return add(u, &e); }
        return 0;
    }
    if (!strcmp(dir, "PS3_GAME")) {
        if (c->mode == 3) return 1;
        e.name = "PARAM.SFO"; e.size = SFOLEN;
        return add(u, &e);
    }
    return 0;
}

static int rf_read(void *ctx, const char *path, uint64_t off, void *buf, uint32_t len)
{
    rf_ctx *c = (rf_ctx *)ctx;
    unsigned char *p = (unsigned char *)buf;
    uint32_t i;
    if (!strcmp(path, "PS3_GAME/PARAM.SFO")) {
        if (c->mode == 4) return -1;
        for (i = 0; i < len; i++) { uint64_t o = off + i; p[i] = o < SFOLEN ? SFO[o] : 0; }
        return 0;
    }
    if (!strcmp(path, "BAD.BIN")) return -1;
    if (!strcmp(path, "A.BIN")) { for (i = 0; i < len; i++) p[i] = pat(off + i + (c->mode == 9 ? 1000 : 0)); return 0; }
    return -1;
}

static int fail_sink(void *ctx, const void *buf, uint32_t len) { (void)ctx; (void)buf; (void)len; return -1; }

typedef struct { ull h, n; } fnv_t;
static int fnv_sink(void *ctx, const void *buf, uint32_t len)
{
    fnv_t *f = (fnv_t *)ctx;
    const unsigned char *a = (const unsigned char *)buf;
    uint32_t i;
    for (i = 0; i < len; i++) { f->h ^= a[i]; f->h *= 1099511628211ull; }
    f->n += len;
    return 0;
}

static int expect_plan(int mode, int want)
{
    rf_ctx c;
    ps3iso_src src;
    ps3iso_build *b = NULL;
    uint64_t total;
    int rc;
    c.mode = mode;
    memset(&src, 0, sizeof src);
    src.ctx = &c; src.list = rf_list; src.read = rf_read; src.root_mtime = 1700000000; src.creation_time = 1700000000;
    rc = ps3iso_build_plan(&src, &b, &total);
    printf("  mode %d: plan rc=%d want %d (%s)\n", mode, rc, want, b ? ps3iso_build_msg(b) : "");
    ps3iso_build_free(b);
    return rc == want;
}

static int cmd_refuse(void)
{
    int ok = 1;
    rf_ctx c;
    ps3iso_src src;
    ps3iso_build *b = NULL, *b2 = NULL, *b3 = NULL;
    uint64_t total = 0, total2 = 0, total3 = 0;
    fnv_t f1, f2, f3, f4;
    int rc;
    init_sfo();
    ok &= expect_plan(0, PS3ISO_E_NAME_TOO_LONG);
    ok &= expect_plan(1, PS3ISO_E_SPLIT666);
    ok &= expect_plan(2, PS3ISO_E_NO_SFO);
    ok &= expect_plan(3, PS3ISO_E_LIST);
    ok &= expect_plan(4, PS3ISO_E_READ);
    ok &= expect_plan(8, PS3ISO_OK);
    /* emit-time failures */
    c.mode = 8;
    memset(&src, 0, sizeof src);
    src.ctx = &c; src.list = rf_list; src.read = rf_read; src.root_mtime = 1700000000; src.creation_time = 1700000000;
    rc = ps3iso_build_plan(&src, &b, &total);
    if (rc) ok = 0;
    else {
        rc = ps3iso_build_emit(b, fail_sink, NULL);
        printf("  failing sink: emit rc=%d want %d\n", rc, PS3ISO_E_SINK);
        if (rc != PS3ISO_E_SINK) ok = 0;
    }
    ps3iso_build_free(b); b = NULL;
    c.mode = 6;
    rc = ps3iso_build_plan(&src, &b, &total);
    if (rc) ok = 0;
    else {
        fnv_t t = { 14695981039346656037ull, 0 };
        rc = ps3iso_build_emit(b, fnv_sink, &t);
        printf("  failing read: emit rc=%d want %d\n", rc, PS3ISO_E_READ);
        if (rc != PS3ISO_E_READ) ok = 0;
    }
    ps3iso_build_free(b); b = NULL;
    /* two builds alive at once, different content, emitted interleaved; same plan emitted twice */
    f1.h = f2.h = f3.h = f4.h = 14695981039346656037ull; f1.n = f2.n = f3.n = f4.n = 0;
    c.mode = 8;
    rc = ps3iso_build_plan(&src, &b, &total);
    if (rc) ok = 0;
    {
        rf_ctx c2;
        ps3iso_src src2 = src;
        c2.mode = 9;
        src2.ctx = &c2;
        if (ps3iso_build_plan(&src2, &b2, &total2)) ok = 0;
        if (ps3iso_build_plan(&src, &b3, &total3)) ok = 0;
        if (b && b2 && b3) {
            if (ps3iso_build_emit(b2, fnv_sink, &f2)) ok = 0;
            if (ps3iso_build_emit(b, fnv_sink, &f1)) ok = 0;
            if (ps3iso_build_emit(b, fnv_sink, &f4)) ok = 0;
            if (ps3iso_build_emit(b3, fnv_sink, &f3)) ok = 0;
        }
        printf("  two builds: A=%016llx B=%016llx A again=%016llx A'=%016llx sizes %llu/%llu/%llu\n", f1.h, f2.h, f4.h, f3.h, f1.n, f2.n, f3.n);
        if (f1.h == f2.h || f1.h != f3.h || f1.h != f4.h || f1.n != total || f2.n != total2 || f3.n != total3) ok = 0;
    }
    ps3iso_build_free(b); ps3iso_build_free(b2); ps3iso_build_free(b3);
    printf("VERDICT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "judge-build")) return cmd_judge_build(argv[2], argv[3], argc > 4 && !strcmp(argv[4], "--first-fail"));
    if (argc >= 4 && !strcmp(argv[1], "build")) return cmd_build(argv[2], argv[3], argc > 4 ? atoll(argv[4]) : 0);
    if (argc >= 4 && !strcmp(argv[1], "unpack")) return cmd_unpack(argv[2], argv[3]);
    if (argc >= 2 && !strcmp(argv[1], "big")) return cmd_big();
    if (argc >= 2 && !strcmp(argv[1], "refuse")) return cmd_refuse();
    fprintf(stderr, "usage: judge-build SRC REF [--first-fail] | build SRC OUT [creation] | unpack ISO OUTDIR | big\n");
    return 2;
}
