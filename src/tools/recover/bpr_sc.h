/* bpr_sc.h -- the .bpr recovery file (sidecar), format version 1.
 *
 * Layout, groups and checksums are specified in the recovery plan, section 19;
 * test/sidecar_model.py is an independent implementation of the same text.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */
#ifndef BPR_SC_H
#define BPR_SC_H

#include "bpr.h"

#define BPR_SC_MIN_SS      64u
#define BPR_SC_MAX_SS      (1u << 24)
#define BPR_SC_DEFAULT_SS  65536u
#define BPR_SC_CHUNK       1024u         /* table entries per checksummed chunk */
#define BPR_SC_FIXED       80u
#define BPR_SC_FOOTER      48u
#define BPR_SC_MAX_SECTORS (1u << 25)    /* data + parity sectors in one sidecar */
#define BPR_SC_MAX_NAME    255u
#define BPR_SC_MAX_PARITY_MEM (256u << 20)  /* r sectors are held in memory at once */

/* ---- bpr_gf.c --------------------------------------------------------- */
void        bpr_gf_init(void);
const char *bpr_gf_kernel_name(void);
uint8_t     bpr_gf_mul(uint8_t a, uint8_t b);
uint8_t     bpr_gf_inv(uint8_t a);
void        bpr_gf_muladd(uint8_t *dst, const uint8_t *src, size_t n, uint8_t c);
uint8_t     bpr_gf_cauchy(unsigned r, unsigned j, unsigned k);
int         bpr_gf_invert(uint8_t *m, uint8_t *inv, unsigned n);

/* ---- bpr_sc.c --------------------------------------------------------- */
typedef struct {
    uint64_t size;                  /* recorded size of the volume file */
    char     name[BPR_SC_MAX_NAME + 1];
    uint32_t first;                 /* first data sector of this volume */
    uint32_t nsec;
} bpr_sc_vol;

typedef struct {
    uint64_t    set_id;
    uint32_t    nvol, ss, S, r, ngroups, nparity;
    uint64_t    t_off, p_off, h2_off, t2_off;
    bpr_sc_vol *vol;
    uint8_t    *tab;                /* (S + nparity) entries of 12 bytes */
    uint8_t    *chunk;              /* per table chunk: 0 unknown, 1 T, 2 T2 */
    uint32_t    nchunks;
    uint64_t    file_size;
    int         hdr_from_copy;
    uint64_t    hdr_len, table_len;
    bpr_file   *f;                  /* the sidecar, kept open for parity reads */
} bpr_sc;

/* Percent in permille (10.0% = 100): groups for S data sectors. */
void     bpr_sc_params(uint32_t S, unsigned permille, uint32_t *ngroups,
                       uint32_t *G, uint32_t *r);
uint64_t bpr_sc_hash(uint32_t idx, const uint8_t *data, uint32_t ss);
uint32_t bpr_sc_group_size(uint32_t S, uint32_t ngroups, uint32_t g);
/* Is name a safe base name for a file the tool will create? */
int      bpr_sc_valid_name(const char *name, size_t len);

/* Table entry accessors.  Entries of unknown chunks must not be used. */
int      bpr_sc_known(const bpr_sc *sc, uint32_t idx);
/* 1 data matches the table, 0 it does not, -1 the entry is unknown. */
int      bpr_sc_check(const bpr_sc *sc, uint32_t idx, const uint8_t *data);

/* Serialisation, shared by protect.  Return malloc'd buffers. */
uint8_t *bpr_sc_header_bytes(const bpr_sc *sc, size_t *len);
uint8_t *bpr_sc_table_bytes(const bpr_sc *sc, size_t *len);
void     bpr_sc_footer_bytes(const bpr_sc *sc, uint8_t out[BPR_SC_FOOTER]);
/* Section 19.4 offsets for a header describing the volumes; 0 ok. */
int      bpr_sc_layout(bpr_sc *sc);
/* Section 19.2 limits on the group numbers; 0 ok, else err says why. */
int      bpr_sc_geometry(const bpr_sc *sc, char *err, size_t errn);
void     bpr_sc_set_entry(bpr_sc *sc, uint32_t idx, const uint8_t *data);

/* Parse and validate; err gets a reason on failure.  0 ok, -1 unusable. */
int      bpr_sc_load(bpr_sc *sc, const char *path, char *err, size_t errn);
void     bpr_sc_free(bpr_sc *sc);
/* Read parity sector p into buf (ss bytes).  0 ok. */
int      bpr_sc_read_parity(const bpr_sc *sc, uint32_t p, uint8_t *buf);

/* ---- commands (bpr_sidecar.c) ----------------------------------------- */
int bpr_cmd_protect(int argc, char **argv);
int bpr_cmd_check(int argc, char **argv);
int bpr_cmd_repair(int argc, char **argv);

#endif
