/*
 * bp_net.h -- local-network send/receive for BoltPack (POSIX frontends)
 *
 * Frontend-layer feature only; NOT part of the portable archive core.
 * Compiled only when BOLTPACK_NET is defined (32-bit+ POSIX targets).
 *
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the GNU General Public License v2
 */

#ifndef BP_NET_H
#define BP_NET_H

#include "../../core/bp_types.h"

/* Well-known ports (unassigned by IANA). */
#define BP_NET_UDP_PORT   41747   /* discovery */
#define BP_NET_TCP_PORT   41748   /* transfer */

#define BP_NET_MAGIC      0x424F4C54UL   /* 'BOLT' */
#define BP_NET_VERSION    1

/* Opcodes (wire, big-endian). */
#define BP_NET_OP_QUERY     0x0001
#define BP_NET_OP_RESPONSE  0x0002
#define BP_NET_OP_ANNOUNCE  0x0010
#define BP_NET_OP_ACCEPT    0x0020
#define BP_NET_OP_REJECT    0x0021
#define BP_NET_OP_SUCCESS   0x0030
#define BP_NET_OP_FAILURE   0x0031

#define BP_NET_NAME_MAX     28    /* friendly name bytes on the wire */
#define BP_NET_FNAME_MAX    63    /* archive base-name bytes on the wire */

/* A receiver discovered on the local network. */
typedef struct {
    char ip[46];                      /* dotted-quad (room for IPv6 too) */
    char name[BP_NET_NAME_MAX + 1];   /* friendly name, NUL-terminated */
    int  tcp_port;
    int  auto_accept;                 /* 1 = receiver auto-accepts */
} bp_net_peer_t;

/*
 * Host/UI callbacks so the same module serves both the CLI and GUI
 * frontends.  bp_net.c never touches stdin/stdout/stderr directly; every
 * decision, progress update, and human-facing message goes through here.
 * Callbacks may run on a worker thread.  Any pointer may be NULL:
 *   decide   NULL -> accept;  progress/message NULL -> silent;
 *   extract_io NULL -> extraction is treated as unsupported.
 */
typedef struct bp_net_ui {
    /* Accept an incoming transfer?  Return 1 to accept, 0 to reject. */
    int  (*decide)(const char *peer_name, const char *peer_ip,
                   const char *fname, double size_bytes, void *ud);
    /* Transfer progress.  label is "sending" or "receiving";
     * finished != 0 on the final call for a transfer. */
    void (*progress)(const char *label, double done, double total,
                     int finished, void *ud);
    /* Human-facing message; is_error selects severity/stream. */
    void (*message)(int is_error, const char *text, void *ud);
    /* Extract a verified archive (path names already safety-checked). */
    int  (*extract_io)(bp_io_t *aio, const char *out_dir, int force,
                       void *ud);
    /* Polled by the listen loop; non-zero requests a clean shutdown. */
    volatile int *stop;
    void *ud;                         /* opaque host context */
} bp_net_ui_t;

/* Broadcast a discovery query and collect responses.
 * Fills peers[0..max_peers-1]; returns the count found, or -1 on error. */
int bp_net_scan(bp_net_peer_t *peers, int max_peers, int timeout_sec);

/* Run the receiver loop (blocks until interrupted).
 * name:        friendly name to advertise (NULL = system hostname).
 * auto_accept: 1 = accept every transfer, 0 = prompt per transfer.
 * do_extract:  1 = extract contents, 0 = save the .bolt file (default).
 * verify:      1 = re-verify the saved .bolt on disk, 0 = skip that pass.
 * out_dir:     destination directory (NULL = current directory).
 * ui:          host callbacks (must be non-NULL).
 * Returns 0 on clean exit, non-zero on setup failure. */
int bp_net_listen(const char *name, int auto_accept, int do_extract,
                  int verify, const char *out_dir, int force,
                  const bp_net_ui_t *ui);

/* Send an already-built archive file to a receiver.
 * target:      friendly name (resolved via a brief scan) or dotted-quad IP.
 * archive_path: path to the completed .bolt file to stream.
 * sender_name: our friendly name for the ANNOUNCE (NULL = hostname).
 * ui:          host callbacks (must be non-NULL).
 * Returns 0 on success, non-zero on failure. */
int bp_net_send(const char *target, const char *archive_path,
                const char *sender_name, const bp_net_ui_t *ui);

/* Implemented in main.c: extract an open archive using the frontend's
 * existing extract callbacks.  Declared here so bp_net.c can reuse them
 * without duplicating the filesystem logic. */
int bp_frontend_extract_io(bp_io_t *aio, const char *out_dir, int force);

#endif /* BP_NET_H */
