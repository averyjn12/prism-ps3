/*
 * ps3iso_core.h - PS3 ISO builder (folder -> ISO) and unpacker (ISO -> folder).
 *
 * Portable C99, no globals, no OS calls: every file/dir/sink operation goes
 * through callbacks, so the same code runs on the PS3 (PSL1GHT) and on Linux.
 * The layout is a port of bucanero/ps3iso-utils makeps3iso.c (GPL-3.0, same
 * licence as Prism): sectors 0/1 PS3 header, ISO 9660 primary + Joliet
 * supplementary descriptors, L/M path tables, directory records, file data.
 *
 * What differs from makeps3iso (on purpose):
 *   - entries keep the order the list callback gives them (makeps3iso keeps
 *     readdir order); sort in the callback if you want a stable ISO.
 *   - the 0x1B0 + 0x10 "random" bytes at 0x840 / 0x9F0 are deterministic
 *     (xorshift32 from src.seed).
 *   - dates come from the callbacks' mtime values, the volume creation date
 *     from src.creation_time (UTC calendar fields of the number given).
 *   - names longer than 213 bytes or 105 UTF-16 units are REFUSED (makeps3iso allows
 *     222 but then writes directory records over 255 bytes, which are corrupt).
 *   - names with characters above U+FFFF (4-byte UTF-8): makeps3iso writes a wrong
 *     surrogate pair (no - 0x10000); ours writes the correct one.
 *   - more than 4096 directories are accepted (limit 65535, the path-table
 *     parent field is 16 bit).
 *   - files named *.666NN (makeps3iso's pre-split files) are REFUSED with
 *     PS3ISO_E_SPLIT666; makeps3iso would merge/ignore them.
 * Files larger than 0xFFFFF800 bytes are written as multi-extent files exactly
 * like makeps3iso (several records, flag 0x80 on all but the last, extents
 * back to back); the unpacker reads them back.
 * Files inside a directory whose name ends in PS3_UPDATE are NOT put in the
 * ISO (makeps3iso's NOPS3_UPDATE), but the directory still reserves room for
 * their records, as makeps3iso does.
 */
#ifndef PS3ISO_CORE_H
#define PS3ISO_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS3ISO_SECTOR 2048u

/* return codes: 0 = ok, negative = error */
#define PS3ISO_OK               0
#define PS3ISO_E_ARG           -1
#define PS3ISO_E_NOMEM         -2
#define PS3ISO_E_LIST          -3   /* list callback failed */
#define PS3ISO_E_READ          -4   /* read callback failed / file shorter than listed */
#define PS3ISO_E_SINK          -5   /* sink/out callback failed */
#define PS3ISO_E_NO_SFO        -6   /* PS3_GAME/PARAM.SFO missing or has no TITLE/TITLE_ID */
#define PS3ISO_E_NAME_TOO_LONG -7   /* name > 222 bytes or > 222 UTF-16 units */
#define PS3ISO_E_TOO_MANY_DIRS -8
#define PS3ISO_E_DIR_FULL      -9   /* directory records did not fit the reserved sectors */
#define PS3ISO_E_SPLIT666     -10
#define PS3ISO_E_TOO_DEEP     -11
#define PS3ISO_E_TOO_BIG      -12   /* more than 2^32-1 sectors */
#define PS3ISO_E_STATE        -13   /* internal consistency check failed */
#define PS3ISO_E_BAD_ISO      -20   /* unpacker: image is inconsistent (see msg) */

const char *ps3iso_strerror(int code);

/* ------------------------------------------------------------------ */
/* Builder                                                             */
/* ------------------------------------------------------------------ */

typedef struct ps3iso_entry {
    const char *name;     /* UTF-8, no path, valid only during the add call */
    int         is_dir;
    uint64_t    size;     /* bytes, files only */
    int64_t     mtime;    /* seconds; the record shows the UTC calendar fields of this number */
} ps3iso_entry;

typedef int (*ps3iso_add_fn)(void *u, const ps3iso_entry *e);

typedef struct ps3iso_src {
    void *ctx;
    /* Call add(u, &entry) once for every entry of directory `dir`
     * ("" = root, otherwise "PS3_GAME/USRDIR", no leading/trailing slash).
     * "." and ".." need not be skipped.  Return 0, or non-zero to fail. */
    int (*list)(void *ctx, const char *dir, ps3iso_add_fn add, void *u);
    /* Read exactly len bytes of file `path` at offset off. Return 0 on success. */
    int (*read)(void *ctx, const char *path, uint64_t off, void *buf, uint32_t len);
    int64_t  root_mtime;     /* date shown for the root directory */
    int64_t  creation_time;  /* volume creation date (PVD/SVD) */
    uint32_t seed;           /* seed of the "random" header bytes (0 = default) */
} ps3iso_src;

/* Receives the ISO bytes in order, in chunks of at most 256 KiB. Return 0 on success. */
typedef int (*ps3iso_write_fn)(void *ctx, const void *buf, uint32_t len);

typedef struct ps3iso_build ps3iso_build;

/* Pass 1: scan the tree through src->list, compute the whole layout, return the
 * exact ISO size in *total_size (always a multiple of 2048).  Memory is
 * proportional to the number of files and directories (names, one 40-byte
 * record per entry; the directory records themselves are built at emit time,
 * one header buffer of (first-file-sector * 2048) bytes, ~100 KB for 500 files).
 * On error *out may still be non-NULL: it only carries ps3iso_build_msg();
 * free it with ps3iso_build_free. */
int  ps3iso_build_plan(const ps3iso_src *src, ps3iso_build **out, uint64_t *total_size);

/* Pass 2: emit exactly *total_size bytes to the sink, in order. May be called
 * once per plan. On success the sum of the sink's lengths == the planned size. */
int  ps3iso_build_emit(ps3iso_build *b, ps3iso_write_fn sink, void *sink_ctx);

void ps3iso_build_free(ps3iso_build *b);

/* "BLUS-30386" and the TITLE of PARAM.SFO, valid until ps3iso_build_free. */
const char *ps3iso_build_title_id(const ps3iso_build *b);
const char *ps3iso_build_title(const ps3iso_build *b);
/* makeps3iso's default file name without extension: "<title, 32 chars ascii>-<BLUS-30386>". */
void ps3iso_build_default_name(const ps3iso_build *b, char *buf, size_t bufsize);
/* human readable detail of the last failure of plan/emit on this handle ("" if none) */
const char *ps3iso_build_msg(const ps3iso_build *b);

/* ------------------------------------------------------------------ */
/* Unpacker                                                            */
/* ------------------------------------------------------------------ */

typedef struct ps3iso_rd {
    void *ctx;
    /* read exactly len bytes at byte offset off of the ISO; 0 = success */
    int (*read)(void *ctx, uint64_t off, void *buf, uint32_t len);
    uint64_t size_hint;      /* total ISO size if known (0 = unknown): volume must fit in it */
} ps3iso_rd;

typedef struct ps3iso_out {
    void *ctx;
    int (*mkdir)(void *ctx, const char *relpath);                               /* parent already exists */
    int (*create)(void *ctx, const char *relpath, uint64_t size, void **handle);/* size known up front */
    int (*write)(void *ctx, void *handle, const void *buf, uint32_t len);
    int (*close)(void *ctx, void *handle);                                      /* also called after a failed write */
} ps3iso_out;

typedef struct ps3iso_unpack_stats {
    uint32_t dirs;           /* directories created (root not counted) */
    uint32_t files;
    uint64_t bytes;          /* file bytes written */
    int      joliet;         /* names came from the Joliet tree */
    int      ps3_header;     /* "PlayStation3" found at 0x800 */
    char     title_id[16];   /* from the PS3 header, "" if none */
    char     msg[256];       /* what was inconsistent, with the path */
} ps3iso_unpack_stats;

/* Walk the image (Joliet names when present, else ISO 9660 names without ";1"),
 * recreate the tree through `out`. Stops at the first inconsistency and returns
 * PS3ISO_E_BAD_ISO (msg filled), or another negative code for callback failures. */
int ps3iso_unpack(const ps3iso_rd *rd, const ps3iso_out *out, ps3iso_unpack_stats *st);

#ifdef __cplusplus
}
#endif
#endif
