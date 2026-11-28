/*
 * boltpack-recover -- recover what can be saved from damaged BoltPack
 * archives.  "analyze" is a read-only report; "extract" writes the entries
 * that can be recovered whole.
 *
 * Exit status: 0 everything found and verified, 1 something found but the
 * set is damaged or incomplete, 2 no entry found, 3 usage or I/O error.
 */
#include "bpr.h"

#include <stdlib.h>
#include <string.h>

static void usage(FILE *o)
{
    fputs("Usage: boltpack-recover analyze [options] VOLUME...\n"
          "       boltpack-recover extract [options] -o DIR VOLUME...\n"
          "       boltpack-recover rebuild [options] -o OUT.bolt [--volume-size N] VOLUME...\n"
          "\n"
          "analyze reads damaged BoltPack format 7 archives or volume sets and\n"
          "reports what is found, without changing anything.\n"
          "\n"
          "extract also writes the entries that can be recovered whole into DIR,\n"
          "which must not exist or must be empty.  Files are written under their\n"
          "real names only if every block decoded and the payload checksum matched;\n"
          "otherwise they are kept as NAME.damaged.  An entry with lost blocks is\n"
          "written as NAME.damaged, full length, with the lost bytes zero-filled;\n"
          "the report lists which byte ranges are ok and which are lost.\n"
          "\n"
          "rebuild writes a new, valid archive (a volume set if --volume-size is\n"
          "given) from the entries that verified, including resource-fork and\n"
          "metadata entries, which extract cannot restore.  Blocks are copied, not\n"
          "recompressed, except where a volume boundary splits one.  Entries that\n"
          "are not whole are left out and listed in the report.  OUT must not exist.\n"
          "\n"
          "Options:\n"
          "  -o DIR | -o OUT      extraction directory (extract) or new archive (rebuild)\n"
          "  --volume-size N      rebuild: volumes of at most N bytes (K, M, G suffixes)\n"
          "  --format text|json   report format (default text)\n"
          "  --report FILE        write the report to FILE\n"
          "  --set-id HEX         use only volumes of this set\n"
          "  --taint-min-run N    Fluff: after a lost block, report an ok run shorter\n"
          "                       than N bytes between tainted runs as tainted (64;\n"
          "                       1 reports every run, which can mean millions)\n"
          "  -h, --help           this help\n"
          "\n"
          "VOLUME... is every file of the set, in any order; files are identified\n"
          "by their headers, not their names.\n", o);
}

/* How well an analysis went: a trusted table first, then how many entries
 * verified from header to trailer.  Used to choose between equally well-formed
 * copies of the same volume. */
static long analysis_score(const bpr_ctx *c)
{
    long n = 0;
    size_t k;
    for (k = 0; k < c->nent; k++)
        if (c->ent[k].status == BPR_ST_OK && c->ent[k].confidence != BPR_CONF_SUSPECT)
            n++;
    return n + (c->table.status == BPR_TBL_TRUSTED ? 1000000L : 0);
}

/* Two files that claim the same volume number and look equally good: analyse
 * the set with each of them and keep the copy whose entries verify.  Returns
 * the context to use (the argument, or a better one it replaced). */
static bpr_ctx *choose_ties(bpr_ctx *c, int nfiles, char **files)
{
    int n;
    for (n = 0; n < BPR_MAX_VOLUMES && n <= c->max_number; n++) {
        int k, ties = c->ntied[n];
        for (k = 1; k < ties; k++) {
            bpr_ctx *t = (bpr_ctx *)calloc(1, sizeof(*t));
            if (t == NULL) return c;
            memcpy(t->prefer, c->prefer, sizeof t->prefer);
            t->prefer[n] = k;
            t->taint_min_ok = c->taint_min_ok;
            t->fixed_set_id_given = c->fixed_set_id_given;
            t->fixed_set_id = c->fixed_set_id;
            if (bpr_load_files(t, nfiles, files) != 0 || t->nfiles == 0) {
                bpr_ctx_free(t);
                free(t);
                return c;
            }
            bpr_identify(t);
            if (bpr_analyze(t) == 0 && analysis_score(t) > analysis_score(c)) {
                bpr_ctx_free(c);
                free(c);
                c = t;
            } else {
                bpr_ctx_free(t);
                free(t);
            }
        }
    }
    return c;
}

int bpr_main(int argc, char **argv)
{
    const char *format = "text", *report = NULL;
    char **files;
    int nfiles = 0, i, rc;
    bpr_ctx *c;
    FILE *out;
    unsigned ok = 0, found = 0;
    size_t k;
    int extract = 0, have_x = 0, rebuild = 0, have_ri = 0;
    const char *outdir = NULL;
    uint64_t volume_size = 0;
    bpr_xlist xl;
    bpr_rebuild_info ri;

    if (argc < 2) { usage(stderr); return 3; }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(argv[1], "analyze") == 0) extract = 0;
    else if (strcmp(argv[1], "extract") == 0) extract = 1;
    else if (strcmp(argv[1], "rebuild") == 0) extract = rebuild = 1;
    else {
        fprintf(stderr, "boltpack-recover: unknown command '%s'\n", argv[1]);
        usage(stderr);
        return 3;
    }
    files = (char **)calloc((size_t)argc, sizeof(char *));
    c = (bpr_ctx *)calloc(1, sizeof(*c));
    if (files == NULL || c == NULL) { free(files); free(c); return 3; }
    c->taint_min_ok = 64;
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--format") == 0 && i + 1 < argc) {
            format = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) {
            report = argv[++i];
        } else if (strcmp(argv[i], "--volume-size") == 0 && i + 1 < argc) {
            char *end;
            unsigned long long v = strtoull(argv[++i], &end, 10);
            if (end == argv[i] || v == 0) {
                fprintf(stderr, "boltpack-recover: bad --volume-size\n");
                rc = 3;
                goto done;
            }
            if (*end == 'k' || *end == 'K') { v <<= 10; end++; }
            else if (*end == 'm' || *end == 'M') { v <<= 20; end++; }
            else if (*end == 'g' || *end == 'G') { v <<= 30; end++; }
            if (*end != '\0') {
                fprintf(stderr, "boltpack-recover: bad --volume-size\n");
                rc = 3;
                goto done;
            }
            volume_size = v;
        } else if (strcmp(argv[i], "--taint-min-run") == 0 && i + 1 < argc) {
            char *end;
            unsigned long v = strtoul(argv[++i], &end, 10);
            if (*end != '\0' || v > 1000000ul) {
                fprintf(stderr, "boltpack-recover: bad --taint-min-run\n");
                rc = 3;
                goto done;
            }
            c->taint_min_ok = (unsigned)v;
        } else if (strcmp(argv[i], "--set-id") == 0 && i + 1 < argc) {
            char *end;
            c->fixed_set_id = strtoull(argv[++i], &end, 16);
            if (*end != '\0') {
                fprintf(stderr, "boltpack-recover: bad set id\n");
                rc = 3;
                goto done;
            }
            c->fixed_set_id_given = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            rc = 0;
            goto done;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "boltpack-recover: unknown option '%s'\n", argv[i]);
            rc = 3;
            goto done;
        } else {
            files[nfiles++] = argv[i];
        }
    }
    if (nfiles == 0 || (extract && outdir == NULL) ||
        (strcmp(format, "text") != 0 && strcmp(format, "json") != 0)) {
        usage(stderr);
        rc = 3;
        goto done;
    }
    if (bpr_load_files(c, nfiles, files) != 0) { rc = 3; goto done; }
    if (c->nfiles == 0) {
        fprintf(stderr, "boltpack-recover: no readable input files\n");
        rc = 3;
        goto done;
    }
    bpr_identify(c);
    if (bpr_analyze(c) != 0) {
        fprintf(stderr, "boltpack-recover: out of memory\n");
        rc = 3;
        goto done;
    }
    c = choose_ties(c, nfiles, files);
    if (rebuild) {
        if (bpr_plan(c, &xl, 1) != 0) {
            fprintf(stderr, "boltpack-recover: out of memory\n");
            rc = 3;
            goto done;
        }
        have_x = 1;
        if (bpr_rebuild(c, &xl, outdir, volume_size, &ri) != 0) {
            fprintf(stderr, "boltpack-recover: rebuild failed: %s\n",
                    ri.problem[0] ? ri.problem : "unknown error");
            rc = 3;
            goto done;
        }
        have_ri = 1;
        xl.rebuilt = &ri;
        { size_t q; for (q = 0; q < BPR_X_COUNT; q++) xl.counts[q] = 0;
          for (q = 0; q < xl.n; q++) xl.counts[xl.r[q].outcome]++; }
    } else if (extract) {
        if (bpr_fs_prepare_root(outdir) != 0) {
            fprintf(stderr, "boltpack-recover: %s exists and is not an empty "
                    "directory\n", outdir);
            rc = 3;
            goto done;
        }
        if (bpr_extract(c, outdir, &xl) != 0) {
            fprintf(stderr, "boltpack-recover: extraction failed\n");
            rc = 3;
            goto done;
        }
        have_x = 1;
    }
    out = stdout;
    if (report != NULL) {
        out = bpr_fopen_write(report);
        if (out == NULL) {
            fprintf(stderr, "boltpack-recover: cannot write %s\n", report);
            rc = 3;
            goto done;
        }
    }
    if (strcmp(format, "json") == 0) bpr_report_json(c, have_x ? &xl : NULL, out);
    else bpr_report_text(c, have_x ? &xl : NULL, out);
    if (out != stdout) fclose(out);

    for (k = 0; k < c->nent; k++) {
        found++;
        if (c->ent[k].status == BPR_ST_OK) ok++;
    }
    if (have_x) {
        unsigned got = xl.counts[BPR_X_VERIFIED] + xl.counts[BPR_X_VERIFIED_TABLE] +
                       xl.counts[BPR_X_UNVERIFIED] + xl.counts[BPR_X_CREATED];
        unsigned bad = (unsigned)xl.n - got;
        if (got == 0 && xl.counts[BPR_X_PARTIAL] == 0) rc = 2;
        else if (bad == 0 && c->complete && c->table.status == BPR_TBL_TRUSTED &&
                 ok == found && c->nspan == 0)
            rc = 0;             /* intact: nothing needed repairing or rebuilding */
        else rc = 1;
    } else if (found == 0 && c->table.nrec == 0) rc = 2;
    else if (c->complete && ok == found && c->table.status == BPR_TBL_TRUSTED &&
             c->nspan == 0) rc = 0;
    else rc = 1;
done:
    if (have_ri) bpr_rebuild_info_free(&ri);
    if (have_x) bpr_xlist_free(&xl);
    bpr_ctx_free(c);
    free(c);
    free(files);
    return rc;
}
