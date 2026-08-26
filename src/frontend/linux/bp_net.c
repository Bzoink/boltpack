/*
 * bp_net.c -- local-network send/receive for BoltPack (POSIX frontends)
 *
 * Frontend-layer feature only; the archive core never touches a socket.
 * The transfer model is: the sender builds a complete archive to a local
 * file (as create already does), then streams that file over TCP.  The
 * receiver writes the incoming bytes to a temp file, verifies it, then
 * saves or extracts.  This sidesteps the archiver's backward seeks, which
 * make a raw TCP stream unusable as a direct archive destination.
 *
 * Discovery uses a tiny UDP broadcast query/response.  All wire integers
 * are big-endian (network byte order).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#define _XOPEN_SOURCE 600
#define _POSIX_C_SOURCE 200112L
#define _DEFAULT_SOURCE     /* getifaddrs / struct ifaddrs (glibc, BSD, macOS) */

#include "bp_net.h"

#ifdef BOLTPACK_NET

#include "../../core/archiver.h"
#include "../../core/compress.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>

#define BP_NET_IOBUF  65536

/* ---- bp_io_t adapter for FILE * (local copy; main.c's is static) ------- */

static long net_file_read(void *handle, void *buf, bp_uint32 count)
{
    return (long)fread(buf, 1, (size_t)count, (FILE *)handle);
}

static long net_file_write(void *handle, const void *buf, bp_uint32 count)
{
    return (long)fwrite(buf, 1, (size_t)count, (FILE *)handle);
}

static int net_file_seek(void *handle, bp_off_t offset, int origin)
{
    return fseeko((FILE *)handle, (off_t)offset, origin);
}

static bp_off_t net_file_tell(void *handle)
{
    return (bp_off_t)ftello((FILE *)handle);
}

static void bp_io_from_file_net(bp_io_t *io, FILE *fp)
{
    io->read   = net_file_read;
    io->write  = net_file_write;
    io->seek   = net_file_seek;
    io->tell   = net_file_tell;
    io->handle = fp;
}

/* ---- big-endian byte packing ------------------------------------------- */

static void put_be16(bp_uint8 *p, bp_uint16 v)
{
    p[0] = (bp_uint8)((v >> 8) & 0xFF);
    p[1] = (bp_uint8)(v & 0xFF);
}

static void put_be32(bp_uint8 *p, bp_uint32 v)
{
    p[0] = (bp_uint8)((v >> 24) & 0xFF);
    p[1] = (bp_uint8)((v >> 16) & 0xFF);
    p[2] = (bp_uint8)((v >> 8) & 0xFF);
    p[3] = (bp_uint8)(v & 0xFF);
}

static bp_uint16 get_be16(const bp_uint8 *p)
{
    return (bp_uint16)(((bp_uint16)p[0] << 8) | (bp_uint16)p[1]);
}

static bp_uint32 get_be32(const bp_uint8 *p)
{
    return ((bp_uint32)p[0] << 24) | ((bp_uint32)p[1] << 16) |
           ((bp_uint32)p[2] << 8)  |  (bp_uint32)p[3];
}

/* ---- blocking I/O helpers ---------------------------------------------- */

static int send_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t left = len;
    long n;

    while (left > 0) {
        n = (long)send(fd, p, left, 0);
        if (n <= 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t left = len;
    long n;

    while (left > 0) {
        n = (long)recv(fd, p, left, 0);
        if (n <= 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static void set_timeouts(int fd, int seconds)
{
    struct timeval tv;

    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* ---- path safety (network receive only) -------------------------------- */

/*
 * Reject anything that could escape the extraction directory.  Receiving
 * archives over the network aborts the whole extraction on any unsafe
 * name.  The check itself lives in the core (bp_name_is_safe) and is
 * shared with local extraction.
 */
static int validate_name_cb(const bp_entry_t *entry, void *user_data)
{
    int *unsafe = (int *)user_data;

    if (!bp_name_is_safe(entry->name)) {
        *unsafe = 1;
    }
    return 0;
}

/* ---- host message / progress helpers ----------------------------------- */

static void ui_msg(const bp_net_ui_t *ui, int is_error, const char *text)
{
    if (ui && ui->message) ui->message(is_error, text, ui->ud);
}

static double now_seconds(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0) return 0.0;
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

/* Throttle transfer progress and forward it to the host UI.  Rate, ETA and
 * formatting are the host's concern; this only reports raw byte counts. */
typedef struct {
    double last;
} bp_meter_t;

static void meter_start(bp_meter_t *m)
{
    /* 0 forces the first meter_emit to fire immediately, so the host can
     * timestamp the true start of the transfer (otherwise a fast transfer
     * only ever delivers the final 'finished' tick and measures ~0s). */
    m->last = 0.0;
}

static void meter_emit(bp_meter_t *m, const bp_net_ui_t *ui, const char *label,
                       double done, double total, int finished)
{
    double t = now_seconds();

    if (!finished && (t - m->last) < 0.1) return;       /* throttle */
    m->last = t;
    if (ui && ui->progress) ui->progress(label, done, total, finished, ui->ud);
}

/* ---- discovery --------------------------------------------------------- */

/*
 * Send the discovery query out every broadcast-capable interface.  A plain
 * sendto(255.255.255.255) only leaves via one interface (the default
 * route), so on a multi-homed host it misses other local subnets.  We also
 * send to each interface's directed broadcast address to cover them all.
 */
static void scan_send_queries(int fd, const bp_uint8 *query, size_t qlen)
{
    struct sockaddr_in addr;
    struct ifaddrs *ifs = NULL;
    struct ifaddrs *p;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)BP_NET_UDP_PORT);

    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, query, qlen, 0, (struct sockaddr *)&addr, sizeof(addr));

    if (getifaddrs(&ifs) != 0) return;
    for (p = ifs; p != NULL; p = p->ifa_next) {
        struct sockaddr_in *b;

        if (p->ifa_addr == NULL) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;
        if (!(p->ifa_flags & IFF_BROADCAST)) continue;
        if (p->ifa_broadaddr == NULL) continue;
        b = (struct sockaddr_in *)(void *)p->ifa_broadaddr;
        addr.sin_addr = b->sin_addr;
        sendto(fd, query, qlen, 0, (struct sockaddr *)&addr, sizeof(addr));
    }
    freeifaddrs(ifs);
}

int bp_net_scan(bp_net_peer_t *peers, int max_peers, int timeout_sec)
{
    int fd;
    int on = 1;
    bp_uint8 query[8];
    double deadline;
    int count = 0;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) {
        close(fd);
        return -1;
    }

    put_be32(query, (bp_uint32)BP_NET_MAGIC);
    put_be16(query + 4, (bp_uint16)BP_NET_VERSION);
    put_be16(query + 6, (bp_uint16)BP_NET_OP_QUERY);

    scan_send_queries(fd, query, sizeof(query));

    deadline = now_seconds() + (double)timeout_sec;
    for (;;) {
        fd_set rfds;
        struct timeval tv;
        double remain;
        int sel;

        remain = deadline - now_seconds();
        if (remain <= 0.0) break;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        tv.tv_sec = (long)remain;
        tv.tv_usec = (long)((remain - (double)tv.tv_sec) * 1000000.0);
        sel = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) break;

        {
            bp_uint8 resp[40];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            long n;
            int namelen, j, dup;
            const char *ipstr;

            n = recvfrom(fd, resp, sizeof(resp), 0,
                         (struct sockaddr *)&from, &fromlen);
            if (n < 40) continue;
            if (get_be32(resp) != (bp_uint32)BP_NET_MAGIC) continue;
            if (get_be16(resp + 4) != BP_NET_VERSION) continue;
            if (get_be16(resp + 6) != BP_NET_OP_RESPONSE) continue;
            if (count >= max_peers) continue;

            ipstr = inet_ntoa(from.sin_addr);
            dup = 0;
            for (j = 0; j < count; j++) {
                if (strcmp(peers[j].ip, ipstr) == 0) { dup = 1; break; }
            }
            if (dup) continue;

            memset(&peers[count], 0, sizeof(peers[count]));
            strncpy(peers[count].ip, ipstr, sizeof(peers[count].ip) - 1);
            peers[count].tcp_port = (int)get_be16(resp + 8);
            namelen = resp[10];
            if (namelen > BP_NET_NAME_MAX) namelen = BP_NET_NAME_MAX;
            memcpy(peers[count].name, resp + 11, (size_t)namelen);
            peers[count].name[namelen] = '\0';
            peers[count].auto_accept = resp[39] ? 1 : 0;
            count++;
        }
    }

    close(fd);
    return count;
}

/* ---- receiver ---------------------------------------------------------- */

static void answer_query(int udp_fd, const char *our_name, int auto_accept)
{
    bp_uint8 q[64];
    bp_uint8 resp[40];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    long n;
    int nl;

    n = recvfrom(udp_fd, q, sizeof(q), 0,
                 (struct sockaddr *)&from, &fromlen);
    if (n < 8) return;
    if (get_be32(q) != (bp_uint32)BP_NET_MAGIC) return;
    if (get_be16(q + 4) != BP_NET_VERSION) return;
    if (get_be16(q + 6) != BP_NET_OP_QUERY) return;

    memset(resp, 0, sizeof(resp));
    put_be32(resp, (bp_uint32)BP_NET_MAGIC);
    put_be16(resp + 4, (bp_uint16)BP_NET_VERSION);
    put_be16(resp + 6, (bp_uint16)BP_NET_OP_RESPONSE);
    put_be16(resp + 8, (bp_uint16)BP_NET_TCP_PORT);
    nl = (int)strlen(our_name);
    if (nl > BP_NET_NAME_MAX) nl = BP_NET_NAME_MAX;
    resp[10] = (bp_uint8)nl;
    memcpy(resp + 11, our_name, (size_t)nl);
    resp[39] = (bp_uint8)(auto_accept ? 1 : 0);

    sendto(udp_fd, resp, sizeof(resp), 0,
           (struct sockaddr *)&from, fromlen);
}

static int recv_to_tmpfile(int fd, FILE *tmp, double total,
                           const bp_net_ui_t *ui)
{
    bp_meter_t m;
    char buf[BP_NET_IOBUF];
    double done = 0.0;

    meter_start(&m);
    meter_emit(&m, ui, "receiving", 0.0, total, 0);   /* mark true start */
    while (done < total) {
        size_t want;
        long n;
        double rem = total - done;

        want = sizeof(buf);
        if ((double)want > rem) want = (size_t)rem;
        n = (long)recv(fd, buf, want, 0);
        if (n <= 0) return -1;
        if (fwrite(buf, 1, (size_t)n, tmp) != (size_t)n) return -1;
        done += (double)n;
        meter_emit(&m, ui, "receiving", done, total, 0);
    }
    meter_emit(&m, ui, "receiving", total, total, 1);
    return 0;
}

static void net_mkdirs(const char *path)
{
    char *tmp;
    char *p;

    if (!path || !path[0]) return;
    tmp = (char *)malloc(strlen(path) + 1);
    if (!tmp) return;
    strcpy(tmp, path);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    free(tmp);
}

static int save_received(FILE *tmp, const char *out_dir, const char *fname,
                         int force, char *dest, size_t destsz)
{
    char path[4096];
    FILE *out;
    char buf[BP_NET_IOBUF];
    size_t n;

    if (out_dir && out_dir[0]) {
        net_mkdirs(out_dir);
        snprintf(path, sizeof(path), "%s/%s", out_dir, fname);
    } else {
        snprintf(path, sizeof(path), "%s", fname);
    }

    if (!force) {
        struct stat st;
        int k = 1;
        while (stat(path, &st) == 0 && k < 1000) {
            if (out_dir && out_dir[0]) {
                snprintf(path, sizeof(path), "%s/%s.%d", out_dir, fname, k);
            } else {
                snprintf(path, sizeof(path), "%s.%d", fname, k);
            }
            k++;
        }
    }

    out = fopen(path, "wb");
    if (out == NULL) return -1;
    rewind(tmp);
    while ((n = fread(buf, 1, sizeof(buf), tmp)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(out); return -1; }
    }
    fclose(out);
    snprintf(dest, destsz, "%s", path);
    return 0;
}

/* Re-open a saved archive and verify its integrity on disk.  Catches a
 * bad disk write during save, separate from the in-flight transfer check.
 * Returns BP_OK if the downloaded file passes. */
static int verify_archive_file(const char *path)
{
    FILE *vf;
    bp_io_t aio;
    int rc;

    vf = fopen(path, "rb");
    if (vf == NULL) return BP_ERR_IO;
    bp_io_from_file_net(&aio, vf);
    rc = bp_archive_test(&aio, NULL, NULL);
    fclose(vf);
    return rc;
}

static void handle_transfer(int cfd, int do_extract,
                            int verify, const char *out_dir, int force,
                            const bp_net_ui_t *ui)
{
    bp_uint8 ann[128];
    bp_uint8 dec[8];
    bp_uint8 done_pkt[8];
    bp_uint32 size_hi, size_lo;
    double total;
    int snl, fnl;
    char sender[BP_NET_NAME_MAX + 1];
    char fname[BP_NET_FNAME_MAX + 1];
    char peer_ip[46];
    char msgbuf[4200];
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int accept_it;
    FILE *tmp;
    bp_io_t aio;
    int rc;

    set_timeouts(cfd, 30);

    peer_ip[0] = '\0';
    if (getpeername(cfd, (struct sockaddr *)&peer, &plen) == 0) {
        strncpy(peer_ip, inet_ntoa(peer.sin_addr), sizeof(peer_ip) - 1);
        peer_ip[sizeof(peer_ip) - 1] = '\0';
    }

    if (recv_all(cfd, ann, sizeof(ann)) != 0) { close(cfd); return; }
    if (get_be32(ann) != (bp_uint32)BP_NET_MAGIC ||
        get_be16(ann + 4) != BP_NET_VERSION ||
        get_be16(ann + 6) != BP_NET_OP_ANNOUNCE) {
        close(cfd);
        return;
    }

    size_hi = get_be32(ann + 8);
    size_lo = get_be32(ann + 12);
    total = (double)size_lo + (double)size_hi * 4294967296.0;

    snl = ann[16];
    if (snl > BP_NET_NAME_MAX) snl = BP_NET_NAME_MAX;
    memcpy(sender, ann + 17, (size_t)snl);
    sender[snl] = '\0';

    fnl = ann[45];
    if (fnl > BP_NET_FNAME_MAX) fnl = BP_NET_FNAME_MAX;
    memcpy(fname, ann + 46, (size_t)fnl);
    fname[fnl] = '\0';
    {
        char *slash = strrchr(fname, '/');
        if (slash) memmove(fname, slash + 1, strlen(slash + 1) + 1);
        if (fname[0] == '\0' || !bp_name_is_safe(fname)) {
            strcpy(fname, "received.bolt");
        }
    }

    accept_it = 1;
    if (ui && ui->decide) {
        accept_it = ui->decide(sender[0] ? sender : "?", peer_ip,
                               fname, total, ui->ud);
    }

    put_be32(dec, (bp_uint32)BP_NET_MAGIC);
    put_be16(dec + 4, (bp_uint16)BP_NET_VERSION);
    put_be16(dec + 6, (bp_uint16)(accept_it ? BP_NET_OP_ACCEPT
                                            : BP_NET_OP_REJECT));
    if (send_all(cfd, dec, sizeof(dec)) != 0) { close(cfd); return; }
    if (!accept_it) { close(cfd); return; }

    tmp = tmpfile();
    if (tmp == NULL) { close(cfd); return; }
    if (recv_to_tmpfile(cfd, tmp, total, ui) != 0) {
        ui_msg(ui, 1, "transfer incomplete");
        fclose(tmp);
        close(cfd);
        return;
    }

    rewind(tmp);
    bp_io_from_file_net(&aio, tmp);
    rc = bp_archive_test(&aio, NULL, NULL);

    put_be32(done_pkt, (bp_uint32)BP_NET_MAGIC);
    put_be16(done_pkt + 4, (bp_uint16)BP_NET_VERSION);
    put_be16(done_pkt + 6, (bp_uint16)(rc == BP_OK ? BP_NET_OP_SUCCESS
                                                   : BP_NET_OP_FAILURE));
    send_all(cfd, done_pkt, sizeof(done_pkt));
    close(cfd);

    if (rc != BP_OK) {
        ui_msg(ui, 1, "received archive failed verification");
        fclose(tmp);
        return;
    }

    if (do_extract) {
        int unsafe = 0;
        rewind(tmp);
        bp_io_from_file_net(&aio, tmp);
        bp_archive_list(&aio, validate_name_cb, &unsafe);
        if (unsafe) {
            ui_msg(ui, 1, "refusing to extract - archive contains "
                          "unsafe path(s)");
            fclose(tmp);
            return;
        }
        rewind(tmp);
        bp_io_from_file_net(&aio, tmp);
        if (ui && ui->extract_io) {
            rc = ui->extract_io(&aio, out_dir, force, ui->ud);
        } else {
            rc = BP_ERR_UNSUPPORTED_METHOD;
        }
        if (rc != BP_OK) {
            ui_msg(ui, 1, "extract failed");
        } else {
            snprintf(msgbuf, sizeof(msgbuf), "Extracted '%s' from %s",
                     fname, peer_ip);
            ui_msg(ui, 0, msgbuf);
        }
    } else {
        char dest[4096];
        if (save_received(tmp, out_dir, fname, force, dest, sizeof(dest)) == 0) {
            if (!verify) {
                snprintf(msgbuf, sizeof(msgbuf), "Saved %s (from %s)",
                         dest, peer_ip);
                ui_msg(ui, 0, msgbuf);
            } else if (verify_archive_file(dest) == BP_OK) {
                snprintf(msgbuf, sizeof(msgbuf),
                         "Saved %s (from %s) - verified OK", dest, peer_ip);
                ui_msg(ui, 0, msgbuf);
            } else {
                snprintf(msgbuf, sizeof(msgbuf), "WARNING: saved %s but "
                         "on-disk verification FAILED", dest);
                ui_msg(ui, 1, msgbuf);
            }
        } else {
            ui_msg(ui, 1, "failed to save received archive");
        }
    }
    fclose(tmp);
}

int bp_net_listen(const char *name, int auto_accept, int do_extract,
                  int verify, const char *out_dir, int force,
                  const bp_net_ui_t *ui)
{
    int udp_fd, tcp_fd;
    int on = 1;
    struct sockaddr_in addr;
    char hostname[64];
    char our_name[BP_NET_NAME_MAX + 1];
    char msgbuf[256];
    int rc;

    rc = bp_compress_init();
    if (rc != BP_OK) {
        ui_msg(ui, 1, "compression init failed");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    if (name && name[0]) {
        snprintf(our_name, sizeof(our_name), "%s", name);
    } else {
        if (gethostname(hostname, sizeof(hostname)) != 0) {
            strcpy(hostname, "boltpack");
        }
        hostname[sizeof(hostname) - 1] = '\0';
        snprintf(our_name, sizeof(our_name), "%s", hostname);
    }

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) {
        snprintf(msgbuf, sizeof(msgbuf), "socket(udp): %s", strerror(errno));
        ui_msg(ui, 1, msgbuf);
        return 1;
    }
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)BP_NET_UDP_PORT);
    if (bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(msgbuf, sizeof(msgbuf), "bind(udp): %s", strerror(errno));
        ui_msg(ui, 1, msgbuf);
        close(udp_fd);
        return 1;
    }

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd < 0) {
        snprintf(msgbuf, sizeof(msgbuf), "socket(tcp): %s", strerror(errno));
        ui_msg(ui, 1, msgbuf);
        close(udp_fd);
        return 1;
    }
    setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)BP_NET_TCP_PORT);
    if (bind(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(msgbuf, sizeof(msgbuf), "bind(tcp): %s", strerror(errno));
        ui_msg(ui, 1, msgbuf);
        close(udp_fd);
        close(tcp_fd);
        return 1;
    }
    if (listen(tcp_fd, 4) != 0) {
        snprintf(msgbuf, sizeof(msgbuf), "listen: %s", strerror(errno));
        ui_msg(ui, 1, msgbuf);
        close(udp_fd);
        close(tcp_fd);
        return 1;
    }

    snprintf(msgbuf, sizeof(msgbuf),
             "Listening as '%s' (UDP %d, TCP %d), %s",
             our_name, BP_NET_UDP_PORT, BP_NET_TCP_PORT,
             auto_accept ? "auto-accept" : "prompt");
    ui_msg(ui, 0, msgbuf);

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int maxfd;
        int sel;

        if (ui && ui->stop && *ui->stop) break;

        FD_ZERO(&rfds);
        FD_SET(udp_fd, &rfds);
        FD_SET(tcp_fd, &rfds);
        maxfd = (udp_fd > tcp_fd) ? udp_fd : tcp_fd;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;            /* poll the stop flag twice a second */
        sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (sel == 0) continue;         /* timeout: re-check stop flag */
        if (FD_ISSET(udp_fd, &rfds)) {
            answer_query(udp_fd, our_name, auto_accept);
        }
        if (FD_ISSET(tcp_fd, &rfds)) {
            struct sockaddr_in cli;
            socklen_t clilen = sizeof(cli);
            int cfd = accept(tcp_fd, (struct sockaddr *)&cli, &clilen);
            if (cfd >= 0) {
                handle_transfer(cfd, do_extract, verify, out_dir, force, ui);
            }
        }
    }

    close(udp_fd);
    close(tcp_fd);
    return 0;
}

/* ---- sender ------------------------------------------------------------ */

static int resolve_target(const char *target, char *ip, size_t ipsz,
                          int *port)
{
    struct in_addr tmp;
    bp_net_peer_t peers[64];
    int count, i;

    if (inet_pton(AF_INET, target, &tmp) == 1) {
        strncpy(ip, target, ipsz - 1);
        ip[ipsz - 1] = '\0';
        *port = BP_NET_TCP_PORT;
        return 0;
    }

    count = bp_net_scan(peers, 64, 2);
    for (i = 0; i < count; i++) {
        if (strcmp(peers[i].name, target) == 0) {
            strncpy(ip, peers[i].ip, ipsz - 1);
            ip[ipsz - 1] = '\0';
            *port = peers[i].tcp_port ? peers[i].tcp_port : BP_NET_TCP_PORT;
            return 0;
        }
    }
    return -1;
}

int bp_net_send(const char *target, const char *archive_path,
                const char *sender_name, const bp_net_ui_t *ui)
{
    char ip[46];
    int port = BP_NET_TCP_PORT;
    FILE *f;
    off_t fsize;
    bp_uint32 size_hi, size_lo;
    int fd;
    struct sockaddr_in addr;
    bp_uint8 ann[128];
    bp_uint8 dec[8];
    bp_uint8 done_pkt[8];
    int nl, fnl;
    const char *base;
    char sname[BP_NET_NAME_MAX + 1];
    char msgbuf[256];
    bp_meter_t m;
    char buf[BP_NET_IOBUF];
    size_t rd;
    double done, total;

    signal(SIGPIPE, SIG_IGN);

    if (resolve_target(target, ip, sizeof(ip), &port) != 0) {
        snprintf(msgbuf, sizeof(msgbuf), "cannot find receiver '%s'", target);
        ui_msg(ui, 1, msgbuf);
        return 1;
    }

    f = fopen(archive_path, "rb");
    if (f == NULL) {
        snprintf(msgbuf, sizeof(msgbuf), "cannot open archive to send: %s",
                 archive_path);
        ui_msg(ui, 1, msgbuf);
        return 1;
    }
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return 1; }
    fsize = ftello(f);
    rewind(f);
    if (fsize < 0) { fclose(f); return 1; }
    total = (double)fsize;
    size_lo = (bp_uint32)(fsize & 0xFFFFFFFF);
    size_hi = (bp_uint32)((fsize >> 16) >> 16);  /* safe if off_t is 32-bit */

    base = strrchr(archive_path, '/');
    base = base ? base + 1 : archive_path;

    if (sender_name && sender_name[0]) {
        snprintf(sname, sizeof(sname), "%s", sender_name);
    } else {
        char hn[64];
        if (gethostname(hn, sizeof(hn)) != 0) strcpy(hn, "boltpack");
        hn[sizeof(hn) - 1] = '\0';
        snprintf(sname, sizeof(sname), "%s", hn);
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fclose(f); return 1; }
    set_timeouts(fd, 30);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        snprintf(msgbuf, sizeof(msgbuf), "bad address: %s", ip);
        ui_msg(ui, 1, msgbuf);
        close(fd);
        fclose(f);
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(msgbuf, sizeof(msgbuf), "cannot connect to %s:%d: %s",
                 ip, port, strerror(errno));
        ui_msg(ui, 1, msgbuf);
        close(fd);
        fclose(f);
        return 1;
    }

    memset(ann, 0, sizeof(ann));
    put_be32(ann, (bp_uint32)BP_NET_MAGIC);
    put_be16(ann + 4, (bp_uint16)BP_NET_VERSION);
    put_be16(ann + 6, (bp_uint16)BP_NET_OP_ANNOUNCE);
    put_be32(ann + 8, size_hi);
    put_be32(ann + 12, size_lo);
    nl = (int)strlen(sname);
    if (nl > BP_NET_NAME_MAX) nl = BP_NET_NAME_MAX;
    ann[16] = (bp_uint8)nl;
    memcpy(ann + 17, sname, (size_t)nl);
    fnl = (int)strlen(base);
    if (fnl > BP_NET_FNAME_MAX) fnl = BP_NET_FNAME_MAX;
    ann[45] = (bp_uint8)fnl;
    memcpy(ann + 46, base, (size_t)fnl);
    if (send_all(fd, ann, sizeof(ann)) != 0) {
        ui_msg(ui, 1, "send failed (announce)");
        close(fd);
        fclose(f);
        return 1;
    }

    if (recv_all(fd, dec, sizeof(dec)) != 0) {
        ui_msg(ui, 1, "no response from receiver");
        close(fd);
        fclose(f);
        return 1;
    }
    if (get_be16(dec + 6) != BP_NET_OP_ACCEPT) {
        ui_msg(ui, 1, "receiver rejected the transfer");
        close(fd);
        fclose(f);
        return 1;
    }

    meter_start(&m);
    done = 0.0;
    meter_emit(&m, ui, "sending", 0.0, total, 0);     /* mark true start */
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_all(fd, buf, rd) != 0) {
            ui_msg(ui, 1, "send failed during transfer");
            close(fd);
            fclose(f);
            return 1;
        }
        done += (double)rd;
        meter_emit(&m, ui, "sending", done, total, 0);
    }
    meter_emit(&m, ui, "sending", total, total, 1);
    fclose(f);

    if (recv_all(fd, done_pkt, sizeof(done_pkt)) != 0) {
        ui_msg(ui, 1, "no completion ack from receiver");
        close(fd);
        return 1;
    }
    close(fd);
    if (get_be16(done_pkt + 6) != BP_NET_OP_SUCCESS) {
        ui_msg(ui, 1, "receiver reported transfer failure");
        return 1;
    }
    snprintf(msgbuf, sizeof(msgbuf), "Transfer to %s complete.", ip);
    ui_msg(ui, 0, msgbuf);
    return 0;
}

#else /* !BOLTPACK_NET */

typedef int bp_net_translation_unit_not_empty;

#endif /* BOLTPACK_NET */
