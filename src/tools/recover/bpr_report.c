/*
 * bpr_report.c -- text and JSON reports for boltpack-recover analyze.
 *
 * The JSON form is the interface the tests read; schema version 1:
 *
 *   volumes[]   index, path, size, role, identified_by, set_id, number,
 *               header_ok, trailer_ok, size_ok, last, conflict
 *   set         set_id, volume_count (null if unknown), count_source,
 *               last_volume_found, missing[], complete
 *   end_record  status, volume, offset, at_expected_location, table_volume,
 *               table_offset, table_size, entry_count
 *   table       status, why, entry_count
 *   walk        reached_table: the walk from the start of the set reached the
 *               table without ever breaking or resuming at a scanned hit
 *   entries[]   name, type, method, platform, flags, mtime, original_size,
 *               block_size, volume, offset, end_volume, end_offset, blocks,
 *               status (ok|damaged|table-only), reason, need_volume,
 *               confidence (chained|isolated|suspect), inside, in_table,
 *               table_index, table_conflict
 *   unaccounted[]  volume, start, end
 *   ignored_inside_blocks
 *   summary     counts
 *   rebuild     (rebuild only) files[], entries, then as extraction below
 *   extraction  (extract only) root, counts{outcome: n}, results[]: name, type,
 *               volume, offset, outcome, detail, written, header_rebuilt (rebuild: reencoded);
 *               partial results add bytes_ok, bytes_lost and ranges[]
 *               (start, end, status ok|lost, reason) that tile the payload.
 *               Outcome names: verified, verified-by-table,
 *               complete-unverified, created, partial, skipped-suspect,
 *               skipped-supplementary, skipped-partial,
 *               skipped-duplicate-name, no-header, failed.
 */
#include "bpr.h"

#include <inttypes.h>
#include <string.h>

static const char *role_name(int r)
{
    switch (r) {
    case BPR_ROLE_USED: return "used";
    case BPR_ROLE_DUPLICATE: return "duplicate";
    case BPR_ROLE_STRANGER: return "stranger";
    default: return "unidentified";
    }
}

static const char *by_name(int b)
{
    switch (b) {
    case BPR_BY_HEADER: return "header";
    case BPR_BY_TRAILER: return "trailer";
    case BPR_BY_NAME: return "name";
    default: return "none";
    }
}

static const char *status_name(int s)
{
    switch (s) {
    case BPR_ST_OK: return "ok";
    case BPR_ST_DAMAGED: return "damaged";
    default: return "table-only";
    }
}

static const char *conf_name(int c)
{
    switch (c) {
    case BPR_CONF_CHAINED: return "chained";
    case BPR_CONF_SUSPECT: return "suspect";
    default: return "isolated";
    }
}

static const char *table_name(int s)
{
    switch (s) {
    case BPR_TBL_TRUSTED: return "trusted";
    case BPR_TBL_DAMAGED: return "damaged";
    case BPR_TBL_UNREADABLE: return "unreadable";
    default: return "absent";
    }
}

static void jstr(FILE *o, const uint8_t *s, size_t n)
{
    size_t i;
    fputc('"', o);
    for (i = 0; i < n; i++) {
        uint8_t ch = s[i];
        if (ch == '"' || ch == '\\') fprintf(o, "\\%c", ch);
        else if (ch < 0x20 || ch == 0x7F) fprintf(o, "\\u%04x", ch);
        else fputc(ch, o);
    }
    fputc('"', o);
}

static void jcstr(FILE *o, const char *s)
{
    if (s == NULL) fputs("null", o);
    else jstr(o, (const uint8_t *)s, strlen(s));
}

static void entry_json(FILE *o, const bpr_entry *e, int first)
{
    if (!first) fputs(",\n", o);
    fputs("    {\"name\": ", o);
    jstr(o, e->name, e->name_len);
    fprintf(o, ", \"type\": %u, \"method\": %u, \"platform\": %u, \"flags\": %u,"
            " \"mtime\": %" PRIu64 ", \"original_size\": %" PRIu64
            ", \"block_size\": %" PRIu32 ", \"volume\": %d, \"offset\": %" PRIu64,
            e->entry_type, e->method, e->platform, e->flags, e->mtime,
            e->original_size, e->block_size, e->vol, e->off);
    if (e->status != BPR_ST_TABLE_ONLY)
        fprintf(o, ", \"end_volume\": %d, \"end_offset\": %" PRIu64
                ", \"blocks\": %" PRIu32 ", \"compressed_size\": %" PRIu64
                ", \"checksum\": %" PRIu32,
                e->end_vol, e->end_off, e->blocks, e->comp_size, e->checksum);
    fprintf(o, ", \"status\": \"%s\"", status_name(e->status));
    fputs(", \"reason\": ", o);
    jcstr(o, e->reason);
    if (e->need_volume >= 0) fprintf(o, ", \"need_volume\": %d", e->need_volume);
    if (e->status != BPR_ST_TABLE_ONLY) {
        fprintf(o, ", \"confidence\": \"%s\"", conf_name(e->confidence));
        if (e->suspect_reason != NULL) {
            fputs(", \"suspect_reason\": ", o);
            jcstr(o, e->suspect_reason);
        }
        if (e->inside >= 0) fprintf(o, ", \"inside\": %d", e->inside);
        if (e->in_table >= 0) fprintf(o, ", \"in_table\": %s",
                                      e->in_table ? "true" : "false");
        if (e->table_index >= 0) fprintf(o, ", \"table_index\": %d", e->table_index);
        if (e->table_conflict != NULL) {
            fputs(", \"table_conflict\": ", o);
            jcstr(o, e->table_conflict);
        }
    } else {
        fprintf(o, ", \"table_index\": %d", e->table_index);
    }
    fputc('}', o);
}

void bpr_report_json(const bpr_ctx *c, const bpr_xlist *x, FILE *o)
{
    int i, first = 1;
    size_t k;
    unsigned ok = 0, dam = 0, tonly = 0, susp = 0;
    const bpr_table *t = &c->table;

    fputs("{\n  \"tool\": \"boltpack-recover\", \"schema\": 1, "
          "\"command\": \"analyze\",\n  \"volumes\": [\n", o);
    for (i = 0; i < c->nfiles; i++) {
        const bpr_vol *v = &c->files[i];
        fprintf(o, "%s    {\"index\": %d, \"path\": ", i ? ",\n" : "", i);
        jcstr(o, v->path);
        fprintf(o, ", \"size\": %" PRIu64 ", \"role\": \"%s\", "
                "\"identified_by\": \"%s\", \"number\": ",
                v->size, role_name(v->role), by_name(v->by));
        if (v->number >= 0 && v->role == BPR_ROLE_USED) fprintf(o, "%d", v->number);
        else if (v->number >= 0) fprintf(o, "%d", v->number);
        else fputs("null", o);
        fprintf(o, ", \"set_id\": \"%016" PRIx64 "\", \"header_ok\": %s, "
                "\"trailer_ok\": %s, \"size_ok\": %s, \"last\": %s, "
                "\"conflict\": %s}",
                v->set_id, v->hdr_ok ? "true" : "false",
                v->trl_ok ? "true" : "false", v->size_ok ? "true" : "false",
                v->trl_last ? "true" : "false", v->conflict ? "true" : "false");
    }
    fputs("\n  ],\n  \"set\": {", o);
    if (c->have_set) fprintf(o, "\"set_id\": \"%016" PRIx64 "\", ", c->set_id);
    else fputs("\"set_id\": null, ", o);
    if (c->volume_count >= 0) fprintf(o, "\"volume_count\": %d, ", c->volume_count);
    else fputs("\"volume_count\": null, ", o);
    fprintf(o, "\"count_source\": \"%s\", \"highest_volume\": %d, "
            "\"last_volume_found\": %s, \"missing\": [",
            c->count_source, c->max_number, c->last_found ? "true" : "false");
    for (i = 0; i < c->nmissing; i++) fprintf(o, "%s%d", i ? ", " : "", c->missing[i]);
    fprintf(o, "], \"complete\": %s},\n", c->complete ? "true" : "false");

    if (c->have_end)
        fprintf(o, "  \"end_record\": {\"status\": \"ok\", \"volume\": %d, "
                "\"offset\": %" PRIu64 ", \"at_expected_location\": %s, "
                "\"table_volume\": %d, \"table_offset\": %" PRIu64
                ", \"table_size\": %" PRIu64 ", \"entry_count\": %" PRIu32 "},\n",
                c->end.vol, c->end.off, c->end_at_expected ? "true" : "false",
                c->end.table_volume, c->end.table_offset, c->end.table_size,
                c->end.entry_count);
    else
        fputs("  \"end_record\": {\"status\": \"missing\"},\n", o);
    fprintf(o, "  \"table\": {\"status\": \"%s\", \"why\": ", table_name(t->status));
    jcstr(o, t->why);
    fprintf(o, ", \"entry_count\": %" PRIu32 "},\n", t->count);
    fprintf(o, "  \"walk\": {\"reached_table\": %s},\n",
            c->chain_reached_table ? "true" : "false");

    fputs("  \"entries\": [\n", o);
    for (k = 0; k < c->nent; k++) {
        entry_json(o, &c->ent[k], first);
        first = 0;
        if (c->ent[k].status == BPR_ST_OK) ok++;
        else dam++;
        if (c->ent[k].confidence == BPR_CONF_SUSPECT) susp++;
    }
    for (k = 0; k < t->nrec; k++) {
        if (t->rec[k].status != BPR_ST_TABLE_ONLY) continue;
        entry_json(o, &t->rec[k], first);
        first = 0;
        tonly++;
    }
    fputs("\n  ],\n  \"unaccounted\": [", o);
    for (k = 0; k < c->nspan; k++)
        fprintf(o, "%s\n    {\"volume\": %d, \"start\": %" PRIu64 ", \"end\": %" PRIu64 "}",
                k ? "," : "", c->span[k].vol, c->span[k].s, c->span[k].e);
    fprintf(o, "%s],\n  \"ignored_inside_blocks\": %" PRIu64 ",\n",
            c->nspan ? "\n  " : "", c->ignored_inside_blocks);
    fprintf(o, "  \"summary\": {\"entries_ok\": %u, \"entries_damaged\": %u, "
            "\"entries_table_only\": %u, \"entries_suspect\": %u, "
            "\"unaccounted_spans\": %zu}", ok, dam, tonly, susp, c->nspan);
    if (x != NULL) {
        size_t j;
        int n;
        if (x->rebuilt != NULL) {
            size_t f;
            fputs(",\n  \"rebuild\": {\"files\": [", o);
            for (f = 0; f < x->rebuilt->nfiles; f++) {
                if (f) fputs(", ", o);
                jcstr(o, x->rebuilt->files[f]);
            }
            fprintf(o, "], \"entries\": %u", x->rebuilt->entries);
        } else {
            fputs(",\n  \"extraction\": {\"root\": ", o);
            jcstr(o, x->root);
        }
        fputs(", \"counts\": {", o);
        for (n = 0; n < BPR_X_COUNT; n++)
            fprintf(o, "%s\"%s\": %u", n ? ", " : "", bpr_outcome_name(n),
                    x->counts[n]);
        fputs("},\n    \"results\": [", o);
        for (j = 0; j < x->n; j++) {
            const bpr_xres *r = &x->r[j];
            fprintf(o, "%s\n      {\"name\": ", j ? "," : "");
            jstr(o, r->e->name, r->e->name_len);
            fprintf(o, ", \"type\": %u, \"volume\": %d, \"offset\": %" PRIu64
                    ", \"outcome\": \"%s\", \"detail\": ",
                    r->e->entry_type, r->e->vol, r->e->off,
                    bpr_outcome_name(r->outcome));
            jcstr(o, r->detail);
            fputs(", \"written\": ", o);
            jcstr(o, r->written);
            fprintf(o, ", \"header_rebuilt\": %s", r->rebuilt ? "true" : "false");
            if (x->rebuilt != NULL)
                fprintf(o, ", \"reencoded\": %s", r->reencoded ? "true" : "false");
            if (r->nranges > 0) {
                size_t q;
                fprintf(o, ", \"bytes_ok\": %" PRIu64 ", \"bytes_lost\": %" PRIu64
                        ", \"ranges\": [", r->bytes_ok, r->bytes_lost);
                for (q = 0; q < r->nranges; q++) {
                    fprintf(o, "%s{\"start\": %" PRIu64 ", \"end\": %" PRIu64
                            ", \"status\": \"%s\", \"reason\": ",
                            q ? ", " : "", r->ranges[q].s, r->ranges[q].e,
                            r->ranges[q].ok ? "ok" : "lost");
                    jcstr(o, r->ranges[q].reason);
                    fputc('}', o);
                }
                fputc(']', o);
            }
            fputc('}', o);
        }
        fputs("\n    ]}", o);
    }
    fputs("\n}\n", o);
}

static void name_text(FILE *o, const uint8_t *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) fputc(s[i] < 0x20 || s[i] == 0x7F ? '?' : s[i], o);
}

void bpr_report_text(const bpr_ctx *c, const bpr_xlist *x, FILE *o)
{
    int i;
    size_t k;
    unsigned ok = 0, dam = 0, tonly = 0, susp = 0;
    const bpr_table *t = &c->table;

    fprintf(o, "Volumes (%d file%s):\n", c->nfiles, c->nfiles == 1 ? "" : "s");
    for (i = 0; i < c->nfiles; i++) {
        const bpr_vol *v = &c->files[i];
        fprintf(o, "  %s: ", v->path);
        if (v->role == BPR_ROLE_STRANGER) fputs("belongs to another set", o);
        else if (v->role == BPR_ROLE_UNIDENTIFIED) fputs("not identified", o);
        else fprintf(o, "volume %d%s", v->number,
                     v->role == BPR_ROLE_DUPLICATE ? " (duplicate, not used)" : "");
        fprintf(o, " [by %s; header %s, trailer %s%s]\n", by_name(v->by),
                v->hdr_ok ? "ok" : "bad", v->trl_ok ? "ok" : "bad",
                v->trl_ok && !v->size_ok ? ", size differs" : "");
    }
    if (c->have_set) fprintf(o, "Set %016" PRIx64 ": ", c->set_id);
    else fputs("Set unknown: ", o);
    if (c->volume_count >= 0)
        fprintf(o, "%d volume%s (from %s)", c->volume_count,
                c->volume_count == 1 ? "" : "s", c->count_source);
    else
        fprintf(o, "volume count unknown, highest seen is %d (last volume not found)",
                c->max_number);
    if (c->nmissing) {
        fputs("; missing:", o);
        for (i = 0; i < c->nmissing; i++) fprintf(o, " %d", c->missing[i]);
    }
    fputs(c->complete ? "; complete\n" : "; INCOMPLETE\n", o);
    fprintf(o, "End record: %s\n", c->have_end ? "found" : "not found");
    fprintf(o, "File table: %s", table_name(t->status));
    if (t->why) fprintf(o, " (%s)", t->why);
    fputc('\n', o);
    fprintf(o, "Unbroken walk from the start of the set to the table: %s\n",
            c->chain_reached_table ? "yes" : "no");

    fputs("Entries:\n", o);
    for (k = 0; k < c->nent; k++) {
        const bpr_entry *e = &c->ent[k];
        fprintf(o, "  [%d:%" PRIu64 "] ", e->vol, e->off);
        name_text(o, e->name, e->name_len);
        fprintf(o, "  %" PRIu64 " bytes, %s", e->original_size,
                status_name(e->status));
        if (e->reason) fprintf(o, " (%s)", e->reason);
        if (e->confidence != BPR_CONF_CHAINED)
            fprintf(o, ", %s%s%s%s", conf_name(e->confidence),
                    e->suspect_reason ? " (" : "",
                    e->suspect_reason ? e->suspect_reason : "",
                    e->suspect_reason ? ")" : "");
        if (e->table_conflict) fprintf(o, ", table disagrees on %s", e->table_conflict);
        if (e->in_table == 0) fputs(", not in table", o);
        fputc('\n', o);
        if (e->status == BPR_ST_OK) ok++;
        else dam++;
        if (e->confidence == BPR_CONF_SUSPECT) susp++;
    }
    for (k = 0; k < t->nrec; k++) {
        const bpr_entry *e = &t->rec[k];
        if (e->status != BPR_ST_TABLE_ONLY) continue;
        fprintf(o, "  [table %d:%" PRIu64 "] ", e->vol, e->off);
        name_text(o, e->name, e->name_len);
        fprintf(o, "  %" PRIu64 " bytes, in the table only (%s)\n",
                e->original_size, e->reason ? e->reason : "?");
        tonly++;
    }
    if (c->nspan) {
        fprintf(o, "Unaccounted bytes (%zu range%s):\n", c->nspan,
                c->nspan == 1 ? "" : "s");
        for (k = 0; k < c->nspan && k < 50; k++)
            fprintf(o, "  volume %d: %" PRIu64 "-%" PRIu64 " (%" PRIu64 " bytes)\n",
                    c->span[k].vol, c->span[k].s, c->span[k].e,
                    c->span[k].e - c->span[k].s);
        if (c->nspan > 50) fprintf(o, "  ... and %zu more\n", c->nspan - 50);
    }
    if (c->ignored_inside_blocks)
        fprintf(o, "%" PRIu64 " header-shaped byte pattern%s inside verified data "
                "(not entries)\n", c->ignored_inside_blocks,
                c->ignored_inside_blocks == 1 ? "" : "s");
    fprintf(o, "Summary: %u ok, %u damaged, %u only in the table, %u suspect\n",
            ok, dam, tonly, susp);
    if (x != NULL) {
        size_t j;
        int n;
        if (x->rebuilt != NULL) {
            size_t f;
            fprintf(o, "\nRebuilt archive: %u entr%s in %zu volume file%s:\n",
                    x->rebuilt->entries, x->rebuilt->entries == 1 ? "y" : "ies",
                    x->rebuilt->nfiles, x->rebuilt->nfiles == 1 ? "" : "s");
            for (f = 0; f < x->rebuilt->nfiles; f++)
                fprintf(o, "  %s\n", x->rebuilt->files[f]);
            fputs("Entries (verified = copied into the new archive):\n", o);
        } else {
            fprintf(o, "\nExtraction into %s:\n", x->root);
        }
        for (j = 0; j < x->n; j++) {
            const bpr_xres *r = &x->r[j];
            fputs("  ", o);
            name_text(o, r->e->name, r->e->name_len);
            fprintf(o, ": %s%s", bpr_outcome_name(r->outcome),
                    r->rebuilt ? " (header rebuilt from the table)" : "");
            if (r->detail) fprintf(o, " (%s)", r->detail);
            if (r->nranges > 0) {
                size_t q;
                fprintf(o, " %" PRIu64 " of %" PRIu64 " bytes recovered",
                        r->bytes_ok, r->bytes_ok + r->bytes_lost);
                for (q = 0; q < r->nranges; q++)
                    if (!r->ranges[q].ok)
                        fprintf(o, "\n      lost %" PRIu64 "-%" PRIu64 " (%s)",
                                r->ranges[q].s, r->ranges[q].e,
                                r->ranges[q].reason ? r->ranges[q].reason : "?");
            }
            fputc('\n', o);
        }
        fputs(x->rebuilt ? "Rebuild summary:" : "Extraction summary:", o);
        for (n = 0; n < BPR_X_COUNT; n++)
            if (x->counts[n]) fprintf(o, " %u %s,", x->counts[n], bpr_outcome_name(n));
        fputc('\n', o);
    }
}
