/*
 * bp_net_win.c -- local-network send/receive for BoltPack (Windows frontend)
 *
 * WinSock2 implementation of the same bp_net.h interface used by the POSIX
 * frontends (src/frontend/linux/bp_net.c).  The wire protocol is identical
 * and interoperable: UDP broadcast discovery on BP_NET_UDP_PORT, TCP
 * transfer on BP_NET_TCP_PORT, all integers big-endian.
 *
 * This is a frontend-layer feature only; the archive core never touches a
 * socket.  Unlike core code, the Windows frontend is not built with
 * -ansi -pedantic, so this file may use WinSock/Win32 APIs freely.
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 */

#include "../linux/bp_net.h"

#ifdef BOLTPACK_NET

#include "../../core/archiver.h"
#include "../../core/compress.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#define BP_NET_IOBUF  65536

/* ---- WinSock lifecycle ------------------------------------------------- */

static int net_startup(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
}

static void net_cleanup(void)
{
    WSACleanup();
}

/* ---- bp_io_t adapter for FILE * ---------------------------------------- */

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
    return _fseeki64((FILE *)handle, (__int64)offset, origin);
}

static bp_off_t net_file_tell(void *handle)
{
    return (bp_off_t)_ftelli64((FILE *)handle);
}

static void bp_io_from_file_net(bp_io_t *io, FILE *fp)
{
    io->read   = net_file_read;
    io->write  = net_file_write;
    io->seek   = net_file_seek;
    io->tell   = net_file_tell;
    io->handle = fp;
}

/* ---- temp file (auto-deleted) ------------------------------------------ */

/* Create a temp file opened for read+write; the caller DeleteFileA()s the
 * returned path after fclose().  (Windows has no tmpfile() that reliably
 * lands in a writable directory for non-elevated processes.) */
static FILE *net_tmpfile(char *path_out, size_t pathsz)
{
    char dir[MAX_PATH];
    char path[MAX_PATH];
    DWORD n;

    n = GetTempPathA((DWORD)sizeof(dir), dir);
    if (n == 0 || n > sizeof(dir)) {
        strcpy(dir, ".");
    }
    if (GetTempFileNameA(dir, "bpn", 0, path) == 0) {
        return NULL;
    }
    if (pathsz > 0) {
        _snprintf(path_out, pathsz, "%s", path);
        path_out[pathsz - 1] = '\0';
    }
    return fopen(path, "w+b");
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

static int send_all(SOCKET fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t left = len;
    int n;

    while (left > 0) {
        n = send(fd, p, (int)left, 0);
        if (n <= 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static int recv_all(SOCKET fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t left = len;
    int n;

    while (left > 0) {
        n = recv(fd, p, (int)left, 0);
        if (n <= 0) return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static void set_timeouts(SOCKET fd, int seconds)
{
    DWORD ms = (DWORD)seconds * 1000;

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
}

/* ---- path safety (network receive only) -------------------------------- */

/* The check itself lives in the core (bp_name_is_safe) and is shared
 * with local extraction. */
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
    return (double)GetTickCount64() / 1000.0;
}

typedef struct {
    double last;
} bp_meter_t;

static void meter_start(bp_meter_t *m)
{
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
 * sendto(255.255.255.255) only leaves via the default route, so on a
 * multi-homed host it misses other local subnets.  GetAdaptersAddresses
 * enumerates each up interface; we compute its directed broadcast address
 * from the on-link prefix length and send there too.
 */
static void scan_send_queries(SOCKET fd, const bp_uint8 *query, size_t qlen)
{
    struct sockaddr_in addr;
    IP_ADAPTER_ADDRESSES *list = NULL;
    IP_ADAPTER_ADDRESSES *ad;
    ULONG sz = 15000;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc;
    int tries;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)BP_NET_UDP_PORT);

    addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    sendto(fd, (const char *)query, (int)qlen, 0,
           (struct sockaddr *)&addr, sizeof(addr));

    for (tries = 0; tries < 3; tries++) {
        list = (IP_ADAPTER_ADDRESSES *)malloc(sz);
        if (list == NULL) return;
        rc = GetAdaptersAddresses(AF_INET, flags, NULL, list, &sz);
        if (rc == ERROR_BUFFER_OVERFLOW) {
            free(list);
            list = NULL;
            continue;                       /* sz now holds the needed size */
        }
        break;
    }
    if (list == NULL || rc != ERROR_SUCCESS) {
        free(list);
        return;
    }

    for (ad = list; ad != NULL; ad = ad->Next) {
        IP_ADAPTER_UNICAST_ADDRESS *ua;

        if (ad->OperStatus != IfOperStatusUp) continue;
        if (ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        for (ua = ad->FirstUnicastAddress; ua != NULL; ua = ua->Next) {
            struct sockaddr_in *sin;
            bp_uint32 ip, mask, bcast;
            ULONG prefix;

            if (ua->Address.lpSockaddr == NULL) continue;
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            sin = (struct sockaddr_in *)(void *)ua->Address.lpSockaddr;

            prefix = ua->OnLinkPrefixLength;
            if (prefix == 0 || prefix > 32) continue;
            ip = ntohl(sin->sin_addr.s_addr);
            mask = (prefix >= 32) ? 0xFFFFFFFFUL
                                  : ~((1UL << (32 - prefix)) - 1UL);
            bcast = ip | ~mask;

            addr.sin_addr.s_addr = htonl(bcast);
            sendto(fd, (const char *)query, (int)qlen, 0,
                   (struct sockaddr *)&addr, sizeof(addr));
        }
    }
    free(list);
}

int bp_net_scan(bp_net_peer_t *peers, int max_peers, int timeout_sec)
{
    SOCKET fd;
    BOOL on = TRUE;
    bp_uint8 query[8];
    double deadline;
    int count = 0;

    if (net_startup() != 0) return -1;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) { net_cleanup(); return -1; }
    if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST,
                   (const char *)&on, sizeof(on)) != 0) {
        closesocket(fd);
        net_cleanup();
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
        sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel == 0) break;

        {
            bp_uint8 resp[40];
            struct sockaddr_in from;
            int fromlen = sizeof(from);
            int n;
            int namelen, j, dup;
            const char *ipstr;

            n = recvfrom(fd, (char *)resp, sizeof(resp), 0,
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

    closesocket(fd);
    net_cleanup();
    return count;
}

/* ---- receiver ---------------------------------------------------------- */

static void answer_query(SOCKET udp_fd, const char *our_name, int auto_accept)
{
    bp_uint8 q[64];
    bp_uint8 resp[40];
    struct sockaddr_in from;
    int fromlen = sizeof(from);
    int n;
    int nl;

    n = recvfrom(udp_fd, (char *)q, sizeof(q), 0,
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

    sendto(udp_fd, (const char *)resp, sizeof(resp), 0,
           (struct sockaddr *)&from, fromlen);
}

static int recv_to_tmpfile(SOCKET fd, FILE *tmp, double total,
                           const bp_net_ui_t *ui)
{
    bp_meter_t m;
    char buf[BP_NET_IOBUF];
    double done = 0.0;

    meter_start(&m);
    meter_emit(&m, ui, "receiving", 0.0, total, 0);   /* mark true start */
    while (done < total) {
        size_t want;
        int n;
        double rem = total - done;

        want = sizeof(buf);
        if ((double)want > rem) want = (size_t)rem;
        n = recv(fd, buf, (int)want, 0);
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
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            if (_mkdir(tmp) != 0 && errno != EEXIST) { free(tmp); return; }
            *p = '\\';
        }
    }
    _mkdir(tmp);
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
        _snprintf(path, sizeof(path), "%s\\%s", out_dir, fname);
    } else {
        _snprintf(path, sizeof(path), "%s", fname);
    }
    path[sizeof(path) - 1] = '\0';

    if (!force) {
        struct _stat64 st;
        int k = 1;
        while (_stat64(path, &st) == 0 && k < 1000) {
            if (out_dir && out_dir[0]) {
                _snprintf(path, sizeof(path), "%s\\%s.%d", out_dir, fname, k);
            } else {
                _snprintf(path, sizeof(path), "%s.%d", fname, k);
            }
            path[sizeof(path) - 1] = '\0';
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
    _snprintf(dest, destsz, "%s", path);
    dest[destsz - 1] = '\0';
    return 0;
}

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

static void handle_transfer(SOCKET cfd, int do_extract,
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
    int plen = sizeof(peer);
    int accept_it;
    FILE *tmp;
    char tmp_path[MAX_PATH];
    bp_io_t aio;
    int rc;

    set_timeouts(cfd, 30);

    peer_ip[0] = '\0';
    if (getpeername(cfd, (struct sockaddr *)&peer, &plen) == 0) {
        strncpy(peer_ip, inet_ntoa(peer.sin_addr), sizeof(peer_ip) - 1);
        peer_ip[sizeof(peer_ip) - 1] = '\0';
    }

    if (recv_all(cfd, ann, sizeof(ann)) != 0) { closesocket(cfd); return; }
    if (get_be32(ann) != (bp_uint32)BP_NET_MAGIC ||
        get_be16(ann + 4) != BP_NET_VERSION ||
        get_be16(ann + 6) != BP_NET_OP_ANNOUNCE) {
        closesocket(cfd);
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
    if (send_all(cfd, dec, sizeof(dec)) != 0) { closesocket(cfd); return; }
    if (!accept_it) { closesocket(cfd); return; }

    tmp = net_tmpfile(tmp_path, sizeof(tmp_path));
    if (tmp == NULL) { closesocket(cfd); return; }
    if (recv_to_tmpfile(cfd, tmp, total, ui) != 0) {
        ui_msg(ui, 1, "transfer incomplete");
        fclose(tmp);
        DeleteFileA(tmp_path);
        closesocket(cfd);
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
    closesocket(cfd);

    if (rc != BP_OK) {
        ui_msg(ui, 1, "received archive failed verification");
        fclose(tmp);
        DeleteFileA(tmp_path);
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
            DeleteFileA(tmp_path);
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
            _snprintf(msgbuf, sizeof(msgbuf), "Extracted '%s' from %s",
                      fname, peer_ip);
            msgbuf[sizeof(msgbuf) - 1] = '\0';
            ui_msg(ui, 0, msgbuf);
        }
    } else {
        char dest[4096];
        if (save_received(tmp, out_dir, fname, force, dest, sizeof(dest)) == 0) {
            if (!verify) {
                _snprintf(msgbuf, sizeof(msgbuf), "Saved %s (from %s)",
                          dest, peer_ip);
            } else if (verify_archive_file(dest) == BP_OK) {
                _snprintf(msgbuf, sizeof(msgbuf),
                          "Saved %s (from %s) - verified OK", dest, peer_ip);
            } else {
                _snprintf(msgbuf, sizeof(msgbuf), "WARNING: saved %s but "
                          "on-disk verification FAILED", dest);
                msgbuf[sizeof(msgbuf) - 1] = '\0';
                ui_msg(ui, 1, msgbuf);
                fclose(tmp);
                DeleteFileA(tmp_path);
                return;
            }
            msgbuf[sizeof(msgbuf) - 1] = '\0';
            ui_msg(ui, 0, msgbuf);
        } else {
            ui_msg(ui, 1, "failed to save received archive");
        }
    }
    fclose(tmp);
    DeleteFileA(tmp_path);
}

int bp_net_listen(const char *name, int auto_accept, int do_extract,
                  int verify, const char *out_dir, int force,
                  const bp_net_ui_t *ui)
{
    SOCKET udp_fd, tcp_fd;
    BOOL on = TRUE;
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

    if (net_startup() != 0) {
        ui_msg(ui, 1, "WSAStartup failed");
        return 1;
    }

    if (name && name[0]) {
        _snprintf(our_name, sizeof(our_name), "%s", name);
        our_name[sizeof(our_name) - 1] = '\0';
    } else {
        if (gethostname(hostname, sizeof(hostname)) != 0) {
            strcpy(hostname, "boltpack");
        }
        hostname[sizeof(hostname) - 1] = '\0';
        _snprintf(our_name, sizeof(our_name), "%s", hostname);
        our_name[sizeof(our_name) - 1] = '\0';
    }

    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd == INVALID_SOCKET) {
        ui_msg(ui, 1, "socket(udp) failed");
        net_cleanup();
        return 1;
    }
    setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)BP_NET_UDP_PORT);
    if (bind(udp_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ui_msg(ui, 1, "bind(udp) failed");
        closesocket(udp_fd);
        net_cleanup();
        return 1;
    }

    tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (tcp_fd == INVALID_SOCKET) {
        ui_msg(ui, 1, "socket(tcp) failed");
        closesocket(udp_fd);
        net_cleanup();
        return 1;
    }
    setsockopt(tcp_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char *)&on, sizeof(on));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)BP_NET_TCP_PORT);
    if (bind(tcp_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ui_msg(ui, 1, "bind(tcp) failed");
        closesocket(udp_fd);
        closesocket(tcp_fd);
        net_cleanup();
        return 1;
    }
    if (listen(tcp_fd, 4) != 0) {
        ui_msg(ui, 1, "listen failed");
        closesocket(udp_fd);
        closesocket(tcp_fd);
        net_cleanup();
        return 1;
    }

    _snprintf(msgbuf, sizeof(msgbuf),
              "Listening as '%s' (UDP %d, TCP %d), %s",
              our_name, BP_NET_UDP_PORT, BP_NET_TCP_PORT,
              auto_accept ? "auto-accept" : "prompt");
    msgbuf[sizeof(msgbuf) - 1] = '\0';
    ui_msg(ui, 0, msgbuf);

    for (;;) {
        fd_set rfds;
        struct timeval tv;
        int sel;

        if (ui && ui->stop && *ui->stop) break;

        FD_ZERO(&rfds);
        FD_SET(udp_fd, &rfds);
        FD_SET(tcp_fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 500000;            /* poll the stop flag twice a second */
        sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel == SOCKET_ERROR) break;
        if (sel == 0) continue;         /* timeout: re-check stop flag */
        if (FD_ISSET(udp_fd, &rfds)) {
            answer_query(udp_fd, our_name, auto_accept);
        }
        if (FD_ISSET(tcp_fd, &rfds)) {
            struct sockaddr_in cli;
            int clilen = sizeof(cli);
            SOCKET cfd = accept(tcp_fd, (struct sockaddr *)&cli, &clilen);
            if (cfd != INVALID_SOCKET) {
                handle_transfer(cfd, do_extract, verify, out_dir, force, ui);
            }
        }
    }

    closesocket(udp_fd);
    closesocket(tcp_fd);
    net_cleanup();
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
    __int64 fsize;
    bp_uint32 size_hi, size_lo;
    SOCKET fd;
    struct sockaddr_in addr;
    bp_uint8 ann[128];
    bp_uint8 dec[8];
    bp_uint8 done_pkt[8];
    int nl, fnl;
    const char *base, *base2;
    char sname[BP_NET_NAME_MAX + 1];
    char msgbuf[256];
    bp_meter_t m;
    char buf[BP_NET_IOBUF];
    size_t rd;
    double done, total;

    if (net_startup() != 0) {
        ui_msg(ui, 1, "WSAStartup failed");
        return 1;
    }

    if (resolve_target(target, ip, sizeof(ip), &port) != 0) {
        _snprintf(msgbuf, sizeof(msgbuf), "cannot find receiver '%s'", target);
        msgbuf[sizeof(msgbuf) - 1] = '\0';
        ui_msg(ui, 1, msgbuf);
        net_cleanup();
        return 1;
    }

    f = fopen(archive_path, "rb");
    if (f == NULL) {
        _snprintf(msgbuf, sizeof(msgbuf), "cannot open archive to send: %s",
                  archive_path);
        msgbuf[sizeof(msgbuf) - 1] = '\0';
        ui_msg(ui, 1, msgbuf);
        net_cleanup();
        return 1;
    }
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); net_cleanup(); return 1; }
    fsize = _ftelli64(f);
    rewind(f);
    if (fsize < 0) { fclose(f); net_cleanup(); return 1; }
    total = (double)fsize;
    size_lo = (bp_uint32)(fsize & 0xFFFFFFFF);
    size_hi = (bp_uint32)((fsize >> 32) & 0xFFFFFFFF);

    base = strrchr(archive_path, '/');
    base2 = strrchr(archive_path, '\\');
    if (base2 && (!base || base2 > base)) base = base2;
    base = base ? base + 1 : archive_path;

    if (sender_name && sender_name[0]) {
        _snprintf(sname, sizeof(sname), "%s", sender_name);
        sname[sizeof(sname) - 1] = '\0';
    } else {
        char hn[64];
        if (gethostname(hn, sizeof(hn)) != 0) strcpy(hn, "boltpack");
        hn[sizeof(hn) - 1] = '\0';
        _snprintf(sname, sizeof(sname), "%s", hn);
        sname[sizeof(sname) - 1] = '\0';
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCKET) { fclose(f); net_cleanup(); return 1; }
    set_timeouts(fd, 30);
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        _snprintf(msgbuf, sizeof(msgbuf), "bad address: %s", ip);
        msgbuf[sizeof(msgbuf) - 1] = '\0';
        ui_msg(ui, 1, msgbuf);
        closesocket(fd);
        fclose(f);
        net_cleanup();
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        _snprintf(msgbuf, sizeof(msgbuf), "cannot connect to %s:%d", ip, port);
        msgbuf[sizeof(msgbuf) - 1] = '\0';
        ui_msg(ui, 1, msgbuf);
        closesocket(fd);
        fclose(f);
        net_cleanup();
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
        closesocket(fd);
        fclose(f);
        net_cleanup();
        return 1;
    }

    if (recv_all(fd, dec, sizeof(dec)) != 0) {
        ui_msg(ui, 1, "no response from receiver");
        closesocket(fd);
        fclose(f);
        net_cleanup();
        return 1;
    }
    if (get_be16(dec + 6) != BP_NET_OP_ACCEPT) {
        ui_msg(ui, 1, "receiver rejected the transfer");
        closesocket(fd);
        fclose(f);
        net_cleanup();
        return 1;
    }

    meter_start(&m);
    done = 0.0;
    meter_emit(&m, ui, "sending", 0.0, total, 0);     /* mark true start */
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (send_all(fd, buf, rd) != 0) {
            ui_msg(ui, 1, "send failed during transfer");
            closesocket(fd);
            fclose(f);
            net_cleanup();
            return 1;
        }
        done += (double)rd;
        meter_emit(&m, ui, "sending", done, total, 0);
    }
    meter_emit(&m, ui, "sending", total, total, 1);
    fclose(f);

    if (recv_all(fd, done_pkt, sizeof(done_pkt)) != 0) {
        ui_msg(ui, 1, "no completion ack from receiver");
        closesocket(fd);
        net_cleanup();
        return 1;
    }
    closesocket(fd);
    if (get_be16(done_pkt + 6) != BP_NET_OP_SUCCESS) {
        ui_msg(ui, 1, "receiver reported transfer failure");
        net_cleanup();
        return 1;
    }
    _snprintf(msgbuf, sizeof(msgbuf), "Transfer to %s complete.", ip);
    msgbuf[sizeof(msgbuf) - 1] = '\0';
    ui_msg(ui, 0, msgbuf);
    net_cleanup();
    return 0;
}

#else /* !BOLTPACK_NET */

typedef int bp_net_win_translation_unit_not_empty;

#endif /* BOLTPACK_NET */
