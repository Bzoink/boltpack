/*
 * bpr.h -- shared declarations for boltpack-recover
 *
 * boltpack-recover reads damaged BoltPack format 7 archives and volume sets.
 * It is a separate tool from the BoltPack CLI: the CLI refuses to produce
 * output from an archive that fails validation, this tool reports what can be
 * found in one.  Phase 1 (this file set) is read-only: volume identification,
 * structure scan, and a verifying walk.  Nothing is decoded or extracted.
 *
 * C11, no POSIX in the engine; file access goes through the platform layer.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#ifndef BPR_H
#define BPR_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BPR_VOL_HDR     32u
#define BPR_VOL_TRL     32u
#define BPR_END_REC     48u
#define BPR_ENTRY_FIXED 34u
#define BPR_BLOCK_HDR   12u
#define BPR_ENTRY_TRL   16u
#define BPR_TABLE_HDR   16u
#define BPR_MAX_VOLUMES 1000
#define BPR_MAX_NAME    65501u
#define BPR_MAX_BLOCK   16777216u

#define BPR_MAGIC_VOL   0x544C4F42u  /* "BOLT" */
#define BPR_MAGIC_TRL   0x4C4F5642u  /* "BVOL" */
#define BPR_MAGIC_TBL   0x544C4254u  /* "TBLT" */
#define BPR_MAGIC_END   0x424F4C54u  /* "TLOB" */

/* ---- little-endian access ------------------------------------------- */

static inline uint16_t bpr_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t bpr_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint64_t bpr_le64(const uint8_t *p)
{
    return (uint64_t)bpr_le32(p) | ((uint64_t)bpr_le32(p + 4) << 32);
}

/* ---- CRC-32 (IEEE, zlib chaining: start with 0) ----------------------- */

uint32_t bpr_crc32(uint32_t crc, const void *data, size_t len);

/* ---- platform layer (bpr_platform_*.c) -------------------------------- */

typedef struct bpr_file bpr_file;
bpr_file *bpr_file_open(const char *path);
void      bpr_file_close(bpr_file *f);
uint64_t  bpr_file_size(const bpr_file *f);
/* Read exactly n bytes at off.  0 on success, -1 on error or short read. */
int       bpr_file_read(const bpr_file *f, uint64_t off, void *buf, size_t n);

/* Output side.  Paths are archive-style: '/'-separated, relative to a root the
 * tool created for this run.  The platform layer maps them to the host. */
typedef struct bpr_out bpr_out;
/* 0 if dir is absent or an empty directory (created if absent), -1 otherwise. */
int      bpr_fs_prepare_root(const char *dir);
/* Create every parent directory of name under root; no component may be a
 * symlink or a non-directory.  0 on success. */
int      bpr_fs_make_parents(const char *root, const char *name);
int      bpr_fs_mkdir(const char *root, const char *name);       /* dir entry */
/* Create a new file, failing if the path exists (never follows a symlink). */
bpr_out *bpr_out_create(const char *root, const char *name);
int      bpr_out_write(bpr_out *o, const void *buf, size_t n);
int      bpr_out_close(bpr_out *o);                 /* 0 ok */
void     bpr_out_abort(bpr_out *o);                 /* close, keep the file */
/* Restore mode (set-id bits are never restored) and mtime; flags are the
 * entry flags.  0 ok, 1 partly restored, -1 error. */
int      bpr_fs_restore_meta(const char *root, const char *name,
                             uint16_t flags, uint64_t mtime, int is_symlink);
int      bpr_fs_symlink(const char *root, const char *name, const char *target);
int      bpr_fs_rename(const char *root, const char *from, const char *to);
int      bpr_fs_unlink(const char *root, const char *name);
int      bpr_fs_exists(const char *root, const char *name);

/* Output files for rebuild: created exclusively (never replacing a file),
 * written sequentially with the seek the archive writer needs. */
typedef struct bpr_wfile bpr_wfile;
bpr_wfile *bpr_wfile_create(const char *path);
long      bpr_wfile_read(bpr_wfile *f, void *buf, size_t n);
long      bpr_wfile_write(bpr_wfile *f, const void *buf, size_t n);
int       bpr_wfile_seek(bpr_wfile *f, int64_t off, int origin);   /* 0 ok */
int64_t   bpr_wfile_tell(bpr_wfile *f);
int       bpr_wfile_close(bpr_wfile *f);                           /* 0 ok */
int       bpr_path_exists(const char *path);
/* Rename from to to unless to exists.  0 ok. */
int       bpr_path_rename_new(const char *from, const char *to);
int       bpr_path_remove(const char *path);

/* ---- volumes ---------------------------------------------------------- */

enum { BPR_ROLE_USED = 0, BPR_ROLE_DUPLICATE, BPR_ROLE_STRANGER,
       BPR_ROLE_UNIDENTIFIED };
enum { BPR_BY_NONE = 0, BPR_BY_HEADER, BPR_BY_TRAILER, BPR_BY_NAME };

typedef struct {
    char     *path;
    bpr_file *f;
    uint64_t  size;
    int       hdr_ok, trl_ok;
    int       hdr_version;          /* version field when the magic matched */
    int       size_ok;              /* trailer volume_size == file size */
    int       trl_last;             /* trailer BP_VOLUME_LAST flag */
    int       conflict;             /* header and trailer disagree */
    uint64_t  set_id;
    int       number;               /* -1 unknown */
    int       by;                   /* BPR_BY_* */
    int       name_last;            /* file name says "last volume" */
    int       role;
} bpr_vol;

enum { BPR_K_VOLHDR = 1, BPR_K_VOLTRL, BPR_K_ENTHDR, BPR_K_BLOCK,
       BPR_K_ENTTRL, BPR_K_TABLE, BPR_K_ENDREC };

typedef struct {
    uint64_t s, e;                  /* [s, e) within one volume file */
    int      kind;
    int      entry;
} bpr_iv;

typedef struct {
    bpr_iv *a;
    size_t  n, cap;
    int     dirty;
} bpr_ivs;

typedef struct {
    int      vol;
    uint64_t off;
    uint16_t header_size;
    uint16_t name_len;
    uint8_t *name;                  /* malloc'd, name_len bytes */
    uint64_t original_size;
    uint8_t  method, checksum_type, entry_type, platform;
    uint16_t flags;
    uint64_t mtime;
    uint32_t block_size;
} bpr_cand;

typedef struct {
    int      vol;
    uint64_t off;
    uint64_t set_id;
    int      volume_count, table_volume;
    uint32_t table_crc;
    uint64_t table_offset, table_size;
    uint32_t entry_count;
} bpr_endrec;

enum { BPR_ST_OK = 0, BPR_ST_DAMAGED, BPR_ST_TABLE_ONLY };
enum { BPR_CONF_CHAINED = 0, BPR_CONF_ISOLATED, BPR_CONF_SUSPECT };

typedef struct {
    int      vol;
    uint64_t off;
    uint16_t header_size;
    uint8_t *name;
    uint16_t name_len;
    uint64_t original_size, comp_size;
    uint8_t  method, entry_type, platform;
    uint16_t flags;
    uint64_t mtime;
    uint32_t block_size, checksum;
    uint32_t blocks;                /* verified blocks */
    uint64_t covered_orig;          /* uncompressed bytes in verified blocks */
    uint64_t blocks_comp;           /* sum of 12 + comp over verified blocks */
    int      from_table;            /* header rebuilt from the table record */
    int      end_vol;               /* position after the last verified structure */
    uint64_t end_off;
    int      status;
    const char *reason;
    int      need_volume;           /* volume number for missing-volume */
    int      confidence;
    const char *suspect_reason;     /* why it is suspect, else NULL */
    int      inside;                /* entry index when suspect, else -1 */
    int      in_table;              /* -1 unknown, 0 no, 1 yes */
    int      table_index;
    const char *table_conflict;
    int      pure;                  /* chained from the start of the set through
                                       verified structures only, never restarted
                                       at an isolated hit */
} bpr_entry;

typedef struct {
    int      status;                /* see below */
    const char *why;
    uint32_t count;
    uint64_t total_orig;
    bpr_entry *rec;                 /* records as entries (status TABLE_ONLY) */
    uint32_t nrec;
} bpr_table;

enum { BPR_TBL_ABSENT = 0, BPR_TBL_TRUSTED, BPR_TBL_DAMAGED,
       BPR_TBL_UNREADABLE };

typedef struct { int vol; uint64_t s, e; } bpr_span;

typedef struct {
    bpr_vol  *files;
    int       nfiles;
    bpr_vol  *slot[BPR_MAX_VOLUMES];/* chosen file for each volume number */
    int       max_number;           /* highest identified number, -1 none */
    uint64_t  set_id;
    int       have_set;
    int       volume_count;         /* -1 unknown */
    const char *count_source;       /* "end-record", "trailer", "inferred" */
    int       last_found;
    int       missing[BPR_MAX_VOLUMES];
    int       nmissing;
    int       complete;

    int       have_end;
    bpr_endrec end;
    int       end_at_expected;

    bpr_table table;

    bpr_endrec *endc;               /* CRC-valid end records found by the scan */
    size_t    nendc, capendc;
    bpr_cand *cand;
    size_t    ncand, capcand;
    uint64_t *tblt_hits;            /* (vol<<48)|offset of TBLT magics */
    size_t    ntblt, captblt;

    bpr_entry *ent;
    size_t    nent, capent;

    bpr_span *span;                 /* unaccounted byte ranges */
    size_t    nspan;
    int       nvolumes_present;

    bpr_ivs   iv[BPR_MAX_VOLUMES];  /* accounted structures */
    bpr_ivs   blk[BPR_MAX_VOLUMES]; /* verified block intervals only */

    uint64_t  ignored_inside_blocks;
    int       chain_reached_table;
    int       fixed_set_id_given;
    uint64_t  fixed_set_id;

    /* Volume files that claim the same number and are equally well formed are
     * tied.  prefer[n] says which of them to use (0: the first given); ntied[n]
     * is how many there were.  The caller may analyse each choice and keep the
     * one whose entries verify. */
    int       prefer[BPR_MAX_VOLUMES];
    int       ntied[BPR_MAX_VOLUMES];

    /* Fluff recovery: an ok run shorter than this, between two tainted runs of
     * one block, is reported tainted too (a few literals among copies).  1
     * reports every run. */
    unsigned  taint_min_ok;
} bpr_ctx;

void bpr_ivs_add(bpr_ivs *v, uint64_t s, uint64_t e, int kind, int entry);

/* bpr_volumes.c */
int  bpr_load_files(bpr_ctx *c, int n, char **paths);
void bpr_identify(bpr_ctx *c);
void bpr_ctx_free(bpr_ctx *c);

/* volume geometry */
static inline uint64_t bpr_stream_limit(const bpr_vol *v)
{
    return v->size >= BPR_VOL_TRL ? v->size - BPR_VOL_TRL : 0;
}

/* bpr_scan.c */
int  bpr_scan_all(bpr_ctx *c);
int  bpr_parse_entry_header(const uint8_t *p, size_t avail, bpr_cand *out);

/* bpr_analyze.c */
typedef struct { const char *reason; int need_volume; } bpr_fail;
int  bpr_analyze(bpr_ctx *c);
const bpr_vol *bpr_vol_at(const bpr_ctx *c, int v);
uint64_t bpr_read_limit(const bpr_vol *x);
void bpr_norm_pos(const bpr_ctx *c, int *v, uint64_t *off);
int  bpr_read_struct(const bpr_ctx *c, int v, uint64_t off, void *buf,
                     size_t n, bpr_fail *f, const char *short_reason);
/* Walk one entry's blocks and trailer.  account != 0 records the verified
 * structures as accounted bytes. */
int  bpr_verify_entry(bpr_ctx *c, const bpr_cand *k, int idx, bpr_entry *e,
                      int account);

/* bpr_decode.c */
typedef struct {
    int      method;
    void    *fctx;                  /* Fluff history context, else NULL */
    uint8_t *in, *out;
    size_t   in_cap, out_cap;
} bpr_decoder;
int         bpr_grow(uint8_t **p, size_t *cap, size_t need);
int         bpr_dec_begin(bpr_decoder *d, int method);
void        bpr_dec_end(bpr_decoder *d);
/* Decode d->in[0..comp) into d->out[0..orig).  NULL on success. */
const char *bpr_dec_block(bpr_decoder *d, uint32_t comp, uint32_t orig);

/* bpr_extract.c */
enum { BPR_X_VERIFIED = 0, BPR_X_VERIFIED_TABLE, BPR_X_UNVERIFIED,
       BPR_X_CREATED, BPR_X_PARTIAL, BPR_X_SKIP_SUSPECT, BPR_X_SKIP_SUPPLEMENTARY,
       BPR_X_SKIP_PARTIAL, BPR_X_SKIP_DUPLICATE, BPR_X_NO_HEADER,
       BPR_X_FAILED, BPR_X_COUNT };

typedef struct {
    uint64_t    s, e;               /* [s, e) of the entry's payload */
    int         ok;                 /* 1: decoded and CRC-verified; 0: lost */
    const char *reason;             /* why lost, else NULL */
} bpr_range;

typedef struct {
    const bpr_entry *e;
    bpr_range  *ranges;             /* partial recovery: covers [0, size) */
    size_t      nranges, capranges;
    uint64_t    bytes_ok, bytes_lost;
    int         outcome;
    const char *detail;             /* static string or NULL */
    char       *written;            /* path written, relative to root */
    int         rebuilt;            /* header rebuilt from the table record */
    uint32_t    crc;                /* payload CRC-32 when it was computed */
    int         reencoded;          /* rebuild: blocks re-cut and re-encoded */
} bpr_xres;

typedef struct {
    bpr_xres  *r;
    size_t     n, cap;
    bpr_entry *own;                 /* entries rebuilt from table records */
    size_t     nown;
    const char *root;
    const struct bpr_rebuild_info_s *rebuilt;   /* set by rebuild, else NULL */
    unsigned   counts[BPR_X_COUNT];
} bpr_xlist;

int  bpr_extract(bpr_ctx *c, const char *root, bpr_xlist *x);
/* Classify every entry without writing anything (see bpr_extract.c). */
int  bpr_plan(bpr_ctx *c, bpr_xlist *x, int include_supplementary);
typedef int (*bpr_sink_fn)(void *user, const uint8_t *data, uint32_t len);
/* Decode an entry's blocks from its header, calling sink with the payload;
 * NULL on success, else a static reason.  *crc_out is the payload CRC-32. */
const char *bpr_decode_entry(const bpr_ctx *c, const bpr_entry *e,
                             bpr_sink_fn sink, void *user, uint32_t *crc_out);
int  bpr_entry_whole(const bpr_entry *e);
int  bpr_expected_checksum(const bpr_ctx *c, const bpr_entry *e,
                           uint32_t *want, int *by_table);
uint32_t bpr_infer_block_size(const bpr_ctx *c, const bpr_entry *e);
/* bpr_partial.c: write NAME.damaged for an entry with lost blocks. */
void bpr_extract_partial(bpr_ctx *c, bpr_xlist *x, bpr_xres *r,
                         const bpr_entry *e);
void bpr_xlist_free(bpr_xlist *x);
const char *bpr_outcome_name(int outcome);

/* bpr_rebuild.c: write a new archive from the entries that verified. */
typedef struct bpr_rebuild_info_s {
    char   **files;                 /* volume files written, in order */
    size_t   nfiles;
    unsigned entries;               /* entries in the new archive */
    char     problem[256];          /* why it failed, if it did */
} bpr_rebuild_info;
int  bpr_rebuild(bpr_ctx *c, bpr_xlist *x, const char *out, uint64_t volume_size,
                 bpr_rebuild_info *info);
void bpr_rebuild_info_free(bpr_rebuild_info *i);

/* bpr_report.c */
void bpr_report_text(const bpr_ctx *c, const bpr_xlist *x, FILE *out);
void bpr_report_json(const bpr_ctx *c, const bpr_xlist *x, FILE *out);

#endif
