/*
 * bp_net_cli.h -- shared console UI + CLI integration for bp_net.
 *
 * Included by POSIX CLI frontends (linux/main.c and the standalone
 * haiku/macosx frontends) so the listen/scan/--send command wiring lives in
 * one place.  Everything here is static and frontend-agnostic; the only
 * thing the including frontend must provide is bp_frontend_extract_io()
 * (declared in bp_net.h), which extracts a received archive using that
 * frontend's own extract callbacks.
 *
 * Compiled only when BOLTPACK_NET is defined.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_NET_CLI_H
#define BP_NET_CLI_H

#ifdef BOLTPACK_NET

#include "../linux/bp_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* ---- network options (set during argument parsing) -------------------- */

static const char *g_send_target = NULL; /* --send <host-or-name> */
static const char *g_net_name    = NULL; /* --name <friendly name> */
static int         g_net_auto    = 0;    /* --auto (1) / --prompt (0, default) */
static int         g_net_extract = 0;    /* --autoextract: unpack vs save .bolt */
static int         g_net_verify  = 0;    /* --autoverify: re-verify saved .bolt */
static int         g_net_timeout = 2;    /* --timeout seconds for --scan */

/* ---- console host callbacks for bp_net -------------------------------- */

typedef struct {
    double start;
    int    started;
} cli_net_prog_t;

static cli_net_prog_t g_cli_net_prog = { 0.0, 0 };

static double bp_net_cli_now(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0.0;
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static void net_fmt_bytes(double b, char *buf)
{
    if (b < 1024.0) {
        sprintf(buf, "%.0f B", b);
    } else if (b < 1048576.0) {
        sprintf(buf, "%.1f KB", b / 1024.0);
    } else if (b < 1073741824.0) {
        sprintf(buf, "%.1f MB", b / 1048576.0);
    } else {
        sprintf(buf, "%.2f GB", b / 1073741824.0);
    }
}

static void net_fmt_rate(double bps, char *buf)
{
    if (bps < 1024.0) {
        sprintf(buf, "%.0f B/s", bps);
    } else if (bps < 1048576.0) {
        sprintf(buf, "%.1f KB/s", bps / 1024.0);
    } else {
        sprintf(buf, "%.1f MB/s", bps / 1048576.0);
    }
}

static int cli_net_decide(const char *peer_name, const char *peer_ip,
                          const char *fname, double size_bytes, void *ud)
{
    char sb[32];
    (void)ud;
    net_fmt_bytes(size_bytes, sb);
    if (g_net_auto) {
        printf("Incoming '%s' (%s) from %s [%s] - auto-accept\n",
               fname, sb, peer_name, peer_ip);
        fflush(stdout);
        return 1;
    } else {
        char line[16];
        printf("Accept '%s' (%s) from %s [%s]? [y/N] ",
               fname, sb, peer_name, peer_ip);
        fflush(stdout);
        if (fgets(line, sizeof(line), stdin) != NULL &&
            (line[0] == 'y' || line[0] == 'Y')) {
            return 1;
        }
        return 0;
    }
}

static void cli_net_progress(const char *label, double done, double total,
                             int finished, void *ud)
{
    cli_net_prog_t *st = (cli_net_prog_t *)ud;
    double now = bp_net_cli_now();
    double rate;
    char db[32], rb[32];

    if (!st->started) { st->start = now; st->started = 1; }
    rate = (now > st->start) ? done / (now - st->start) : 0.0;
    net_fmt_bytes(done, db);
    net_fmt_rate(rate, rb);
    if (finished) {
        fprintf(stderr, "\r  %s: %s in %.2fs (%s)            \n",
                label, db, now - st->start, rb);
        st->started = 0;
    } else {
        double pct = (total > 0.0) ? done / total * 100.0 : 0.0;
        double eta = (rate > 0.0 && total > done) ? (total - done) / rate : 0.0;
        fprintf(stderr, "\r  %s: %s (%.0f%%) %s ETA %d:%02d   ",
                label, db, pct, rb, (int)(eta / 60.0), ((int)eta) % 60);
    }
    fflush(stderr);
}

static void cli_net_message(int is_error, const char *text, void *ud)
{
    (void)ud;
    if (is_error) {
        fprintf(stderr, "boltpack: %s\n", text);
    } else {
        printf("%s\n", text);
        fflush(stdout);
    }
}

static int cli_net_extract(bp_io_t *aio, const char *out_dir, int force,
                           void *ud)
{
    (void)ud;
    return bp_frontend_extract_io(aio, out_dir, force);
}

static void cli_net_ui_init(bp_net_ui_t *ui)
{
    memset(ui, 0, sizeof(*ui));
    ui->decide     = cli_net_decide;
    ui->progress   = cli_net_progress;
    ui->message    = cli_net_message;
    ui->extract_io = cli_net_extract;
    ui->stop       = NULL;
    ui->ud         = &g_cli_net_prog;
}

/* ---- commands --------------------------------------------------------- */

static int cmd_listen(const char *out_dir, int force)
{
    bp_net_ui_t ui;
    cli_net_ui_init(&ui);
    return bp_net_listen(g_net_name, g_net_auto, g_net_extract, g_net_verify,
                         out_dir, force, &ui);
}

static int cmd_scan(void)
{
    bp_net_peer_t peers[64];
    int count, i;

    count = bp_net_scan(peers, 64, g_net_timeout);
    if (count < 0) {
        fprintf(stderr, "boltpack: scan failed\n");
        return 1;
    }
    if (count == 0) {
        printf("No receivers found on the local network.\n");
        return 0;
    }
    printf("Receivers found on local network:\n");
    for (i = 0; i < count; i++) {
        printf("  %-15s  %-28s  (%s)\n",
               peers[i].ip, peers[i].name,
               peers[i].auto_accept ? "auto-accept" : "prompt");
    }
    return 0;
}

/* ---- integration helpers (called from the frontend's main) ------------ */

/* Map a command word to a command char: 'L' listen, 'S' scan, 0 otherwise. */
static char bp_net_cli_command(const char *s)
{
    if (strcmp(s, "listen") == 0 || strcmp(s, "--listen") == 0) return 'L';
    if (strcmp(s, "scan") == 0   || strcmp(s, "--scan") == 0)   return 'S';
    return 0;
}

/* Consume a network option at argv[*pi] if present.  Returns 1 if it was a
 * network option (and advances *pi past it), 0 otherwise.  Exits on a
 * malformed option that is missing its required argument. */
static int bp_net_cli_take_option(int *pi, int argc, char **argv)
{
    int i = *pi;
    const char *a = argv[i];

    if (strcmp(a, "--send") == 0) {
        if (i + 1 >= argc) { fprintf(stderr, "boltpack: %s requires an argument\n", a); exit(1); }
        g_send_target = argv[i + 1]; *pi = i + 2; return 1;
    }
    if (strcmp(a, "--name") == 0) {
        if (i + 1 >= argc) { fprintf(stderr, "boltpack: %s requires an argument\n", a); exit(1); }
        g_net_name = argv[i + 1]; *pi = i + 2; return 1;
    }
    if (strcmp(a, "--auto") == 0)    { g_net_auto = 1; *pi = i + 1; return 1; }
    if (strcmp(a, "--prompt") == 0)  { g_net_auto = 0; *pi = i + 1; return 1; }
    if (strcmp(a, "--autoextract") == 0 || strcmp(a, "--extract") == 0) {
        g_net_extract = 1; *pi = i + 1; return 1;
    }
    if (strcmp(a, "--autoverify") == 0) { g_net_verify = 1; *pi = i + 1; return 1; }
    if (strcmp(a, "--timeout") == 0) {
        if (i + 1 >= argc) { fprintf(stderr, "boltpack: %s requires an argument\n", a); exit(1); }
        g_net_timeout = atoi(argv[i + 1]);
        if (g_net_timeout < 1) g_net_timeout = 1;
        *pi = i + 2; return 1;
    }
    return 0;
}

/* Strip --send/--name from the create argument list (they may appear among
 * the file arguments), recording them in the globals.  argv is compacted in
 * place and *pargc updated. */
static void bp_net_cli_strip_create_args(int *pargc, char **argv)
{
    int argc = *pargc;
    int rd, wr = 0;

    for (rd = 0; rd < argc; rd++) {
        if (rd >= 1 && strcmp(argv[rd], "--send") == 0 && rd + 1 < argc) {
            g_send_target = argv[rd + 1]; rd++; continue;
        }
        if (rd >= 1 && strcmp(argv[rd], "--name") == 0 && rd + 1 < argc) {
            g_net_name = argv[rd + 1]; rd++; continue;
        }
        argv[wr++] = argv[rd];
    }
    *pargc = wr;
}

/* After a successful create, stream the archive if --send was given.
 * Returns 0 on success (or if no send requested), non-zero on send failure. */
static int bp_net_cli_maybe_send(const char *archive_name)
{
    bp_net_ui_t ui;
    if (g_send_target == NULL) return 0;
    cli_net_ui_init(&ui);
    return bp_net_send(g_send_target, archive_name, g_net_name, &ui);
}

/* Print the network help section. */
static void bp_net_cli_usage(FILE *f)
{
    fprintf(f,
        "\nNetwork:\n"
        "  listen           Receive archives over the local network\n"
        "  scan             List receivers on the local network\n"
        "  --send HOST      Stream the created archive to a receiver (name/IP)\n"
        "  --name NAME      Friendly name (listen advertises it; send identifies)\n");
    fprintf(f,
        "  --auto           Receiver accepts all transfers (default: prompt)\n"
        "  --prompt         Receiver prompts before each transfer\n"
        "  --autoextract    Receiver unpacks contents (default: save .bolt)\n"
        "  --autoverify     Verify the saved .bolt on disk after download\n"
        "  --timeout N      scan wait time in seconds (default 2)\n");
    fprintf(f,
        "  e.g.  boltpack listen --name MyPC --auto\n"
        "        boltpack create -F backup.bolt --send MyPC folder\n");
}

#endif /* BOLTPACK_NET */
#endif /* BP_NET_CLI_H */
