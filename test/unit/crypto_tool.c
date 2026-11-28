/*
 * crypto_tool.c -- line-protocol driver for src/core/crypto, used by
 * test/check_crypto.py to compare the C code with an independent Python oracle.
 * Built with BP_CRYPTO_TESTING.  Hex fields use "-" for the empty string.
 *
 *   HU    aead outlen data                    -> hex
 *   HK    aead outlen key32 data              -> hex
 *   PACK  kdf aead norm m t lanes salt16      -> 32-byte header prefix, hex
 *   DERIVE kdf aead norm m t lanes salt16 set8 secret [maxm maxt maxl]
 *                                             -> OK kseal knonce verifier | ERR code
 *   SEAL  aead set8 kseal knonce ctr_lo ctr_hi E32 envelope extra pt   (sets the process counter first)
 *                                             -> OK nonce ct tag | ERR code
 *   OPEN  aead set8 kseal envelope extra nonce ct tag   -> OK pt | ERR code
 *   RSEAL aead key nonce ad pt                -> ct||tag   (raw primitive, KATs)
 *   ROPEN aead key nonce ad ct||tag           -> OK pt | FAIL
 *   ARGON pw salt secret ad m t lanes outlen  -> hex       (raw Argon2id, KATs)
 *   AAD   aead set8 envelope extra            -> the AAD bytes, hex
 *   XOF2  data n1 n2                          -> squeeze n1 then n2 (two calls), hex of both
 *   XOFC  chunk n data                        -> absorb in chunks of `chunk` bytes, squeeze n, hex
 *   POOL  import n1 n2 ...                    -> pool with no OS source, import bytes, draw n1, n2, ... (hex)
 *   HINC  aead outlen key|- chunk data        -> incremental hash, data fed in pieces of `chunk` bytes (hex)
 *   BUDGET aead frames bytes_hi bytes_lo revision table_chunks add_frames add_bytes -> 1 | 0 | ERR code
 *   LIMITS aead kind                          -> "min max" of the plaintext of a frame, or ERR code
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bp_crypto.h"
#include "bp_ascon.h"
#include "bp_entropy.h"

static bp_uint8 g_E[32];
static int os_none(bp_uint8 *b, bp_uint32 n) { (void)b; (void)n; return -1; }
static int E_hook(bp_uint8 E[32]) { memcpy(E, g_E, 32); return 0; }

static bp_uint32 unhex(const char *s, bp_uint8 *out, bp_uint32 max)
{
    bp_uint32 n = 0;
    if (strcmp(s, "-") == 0) return 0;
    while (s[0] && s[1] && n < max) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (bp_uint8)v;
        s += 2;
    }
    return n;
}

static void phex(const bp_uint8 *b, bp_uint32 n)
{
    bp_uint32 i;
    if (n == 0) { printf("-"); return; }
    for (i = 0; i < n; i++) printf("%02x", b[i]);
}

static char *tok(char **p)
{
    char *s = *p, *e;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 0 || *s == '\n' || *s == '\r') { *p = s; return NULL; }
    e = s;
    while (*e && *e != ' ' && *e != '\t' && *e != '\n' && *e != '\r') e++;
    if (*e) *e++ = 0;
    *p = e;
    return s;
}

#define MAXB (1u << 20)
static bp_uint8 b1[MAXB], b2[MAXB], b3[MAXB], b4[MAXB];

int main(void)
{
    static char line[(1u << 21) + 64];
    bp_test_E_hook = E_hook;
    while (fgets(line, sizeof line, stdin)) {
        char *p = line, *cmd = tok(&p);
        if (cmd == NULL) continue;
        if (!strcmp(cmd, "HU") || !strcmp(cmd, "HK")) {
            int aead = atoi(tok(&p));
            bp_uint32 ol = (bp_uint32)atoi(tok(&p));
            bp_uint8 key[32];
            bp_part part;
            int rc;
            if (cmd[1] == 'K') unhex(tok(&p), key, 32);
            part.n = unhex(tok(&p), b1, MAXB);
            part.p = b1;
            if (ol > MAXB) { printf("ERR -1\n"); fflush(stdout); continue; }
            rc = cmd[1] == 'K' ? bp_hk(aead, key, &part, 1, b2, ol) : bp_hu(aead, &part, 1, b2, ol);
            if (rc) printf("ERR %d", rc); else phex(b2, ol);
            printf("\n");
        } else if (!strcmp(cmd, "PACK") || !strcmp(cmd, "DERIVE")) {
            bp_keyparams kp;
            bp_uint8 packed[32], salt[16], set8[8];
            bp_kdf_limits lim;
            int has_lim = 0, derive = cmd[0] == 'D', rc;
            kp.kdf = (bp_uint8)atoi(tok(&p)); kp.aead = (bp_uint8)atoi(tok(&p));
            kp.norm_version = (bp_uint8)atoi(tok(&p));
            kp.m_cost_kib = (bp_uint32)strtoul(tok(&p), NULL, 10);
            kp.t_cost = (bp_uint16)atoi(tok(&p)); kp.lanes = (bp_uint8)atoi(tok(&p));
            unhex(tok(&p), salt, 16); memcpy(kp.salt, salt, 16);
            if (!derive) {
                rc = bp_keyparams_pack32(&kp, packed);
                if (rc) printf("ERR %d", rc); else phex(packed, 32);
            } else {
                bp_keys k; bp_uint8 ver[16]; bp_uint32 sl; char *t;
                unhex(tok(&p), set8, 8);
                sl = unhex(tok(&p), b1, MAXB);
                if ((t = tok(&p)) != NULL) {
                    lim.max_m_cost_kib = (bp_uint32)strtoul(t, NULL, 10);
                    lim.max_t_cost = (bp_uint32)atoi(tok(&p)); lim.max_lanes = (bp_uint32)atoi(tok(&p));
                    has_lim = 1;
                }
                rc = bp_keys_derive(&k, ver, &kp, set8, b1, sl, has_lim ? &lim : NULL);
                if (rc) printf("ERR %d", rc);
                else { printf("OK "); phex(k.k_seal, 32); printf(" "); phex(k.k_nonce, 32); printf(" "); phex(ver, 16); }
            }
            printf("\n");
        } else if (!strcmp(cmd, "SEAL")) {
            bp_keys k; bp_uint8 env[8], nonce[24], tag[16], extra[BP_AAD_EXTRA_MAX];
            bp_uint32 el, pl; int rc;
            memset(&k, 0, sizeof k);
            k.aead = atoi(tok(&p));
            unhex(tok(&p), k.set8, 8); unhex(tok(&p), k.k_seal, 32); unhex(tok(&p), k.k_nonce, 32);
            { bp_uint32 lo = (bp_uint32)strtoul(tok(&p), NULL, 10); bp_uint32 hi = (bp_uint32)strtoul(tok(&p), NULL, 10); bp_test_ctr_set(lo, hi); }
            unhex(tok(&p), g_E, 32); unhex(tok(&p), env, 8);
            el = unhex(tok(&p), extra, sizeof extra);
            pl = unhex(tok(&p), b1, MAXB);
            rc = bp_frame_seal(&k, env, extra, el, b1, pl, nonce, b2, tag);
            if (rc) printf("ERR %d", rc);
            else { printf("OK "); phex(nonce, bp_suite_nonce_len(k.aead)); printf(" "); phex(b2, pl); printf(" "); phex(tag, 16); }
            printf("\n");
        } else if (!strcmp(cmd, "OPEN")) {
            bp_keys k; bp_uint8 env[8], nonce[24], tag[16], extra[BP_AAD_EXTRA_MAX];
            bp_uint32 el, cl; int rc;
            memset(&k, 0, sizeof k);
            k.aead = atoi(tok(&p));
            unhex(tok(&p), k.set8, 8); unhex(tok(&p), k.k_seal, 32); unhex(tok(&p), env, 8);
            el = unhex(tok(&p), extra, sizeof extra);
            unhex(tok(&p), nonce, 24); cl = unhex(tok(&p), b1, MAXB); unhex(tok(&p), tag, 16);
            rc = bp_frame_open(&k, env, extra, el, nonce, b1, cl, tag, b2);
            if (rc) printf("ERR %d", rc); else { printf("OK "); phex(b2, cl); }
            printf("\n");
        } else if (!strcmp(cmd, "RSEAL") || !strcmp(cmd, "ROPEN")) {
            int aead = atoi(tok(&p)), seal = cmd[1] == 'S';
            bp_uint8 key[32], nonce[24], tag[16];
            bp_uint32 al, tl, rc;
            unhex(tok(&p), key, 32); unhex(tok(&p), nonce, 24);
            al = unhex(tok(&p), b3, MAXB); tl = unhex(tok(&p), b1, MAXB);
            if (seal) {
                rc = (bp_uint32)bp_test_aead_seal(aead, key, nonce, b3, al, b1, tl, b2, tag);
                if (rc) printf("ERR %d", (int)rc);
                else { memcpy(b2 + tl, tag, 16); phex(b2, tl + 16); }
            } else if (tl < 16) printf("FAIL");
            else {
                rc = (bp_uint32)bp_test_aead_open(aead, key, nonce, b3, al, b1, tl - 16, b1 + tl - 16, b2);
                if (rc) printf("FAIL"); else { printf("OK "); phex(b2, tl - 16); }
            }
            printf("\n");
        } else if (!strcmp(cmd, "ARGON")) {
            bp_uint32 pl, sl, secl, adl, m, t, l, ol; int rc;
            pl = unhex(tok(&p), b1, MAXB); sl = unhex(tok(&p), b2, MAXB);
            secl = unhex(tok(&p), b3, MAXB); adl = unhex(tok(&p), b4, MAXB);
            m = (bp_uint32)strtoul(tok(&p), NULL, 10); t = (bp_uint32)atoi(tok(&p));
            l = (bp_uint32)atoi(tok(&p)); ol = (bp_uint32)atoi(tok(&p));
            { static bp_uint8 out[256]; if (ol > 256) ol = 256;
              rc = bp_test_argon2id_raw(out, ol, b1, pl, b2, sl, b3, secl, b4, adl, m, t, l);
              if (rc) printf("ERR %d", rc); else phex(out, ol); }
            printf("\n");
        } else if (!strcmp(cmd, "AAD")) {
            int aead = atoi(tok(&p)); bp_uint8 set8[8], env[8]; bp_uint32 el, n;
            unhex(tok(&p), set8, 8); unhex(tok(&p), env, 8); el = unhex(tok(&p), b1, MAXB);
            n = bp_test_build_aad(aead, set8, env, b1, el, b2);
            phex(b2, n); printf("\n");
        } else if (!strcmp(cmd, "XOF2")) {
            bp_xof x; bp_uint32 dl, n1, n2;
            dl = unhex(tok(&p), b1, MAXB); n1 = (bp_uint32)atoi(tok(&p)); n2 = (bp_uint32)atoi(tok(&p));
            bp_xof_init(&x); bp_xof_absorb(&x, b1, dl);
            bp_xof_squeeze(&x, b2, n1); bp_xof_squeeze(&x, b2 + n1, n2);
            phex(b2, n1 + n2); printf("\n");
        } else if (!strcmp(cmd, "XOFC")) {
            bp_xof x; bp_uint32 ch = (bp_uint32)atoi(tok(&p)), n = (bp_uint32)atoi(tok(&p)), dl, off = 0;
            dl = unhex(tok(&p), b1, MAXB);
            bp_xof_init(&x);
            while (off < dl) { bp_uint32 t = dl - off < ch ? dl - off : ch; bp_xof_absorb(&x, b1 + off, t); off += t; }
            bp_xof_squeeze(&x, b2, n); phex(b2, n); printf("\n");
        } else if (!strcmp(cmd, "POOL")) {
            bp_uint32 il = unhex(tok(&p), b1, MAXB), tot = 0; char *t;
            bp_entropy_test_reset(); bp_entropy_test_set_os(os_none);
            bp_entropy_import(b1, il);
            {
                int err = 0;
                while ((t = tok(&p)) != NULL) {
                    bp_uint32 n = (bp_uint32)atoi(t);
                    if (bp_random_bytes(b2 + tot, n) != 0) { err = 1; break; }
                    tot += n;
                }
                if (err) printf("ERR"); else phex(b2, tot);
                printf("\n");
            }
            bp_entropy_test_set_os(NULL); bp_entropy_test_reset();
        } else if (!strcmp(cmd, "HINC")) {
            int aead = atoi(tok(&p)); bp_uint32 ol = (bp_uint32)atoi(tok(&p)), ch, dl, off = 0, kl; bp_uint8 key[32]; char *kt; bp_hash_ctx hc; int rc;
            kt = tok(&p); kl = unhex(kt, key, 32);
            ch = (bp_uint32)atoi(tok(&p)); dl = unhex(tok(&p), b1, MAXB);
            rc = bp_hash_init(&hc, aead, kl ? key : NULL, ol);
            if (rc) printf("ERR %d", rc);
            else {
                while (off < dl) { bp_uint32 t = dl - off < ch ? dl - off : ch; bp_hash_update(&hc, b1 + off, t); off += t; }
                bp_hash_final(&hc, b2); phex(b2, ol);
            }
            printf("\n");
        } else if (!strcmp(cmd, "BUDGET")) {
            int aead = atoi(tok(&p)); bp_budget_use u; bp_uint32 af, ab; int rc;
            u.frames = (bp_uint32)strtoul(tok(&p), NULL, 10);
            u.bytes.hi = (bp_uint32)strtoul(tok(&p), NULL, 10); u.bytes.lo = (bp_uint32)strtoul(tok(&p), NULL, 10);
            u.revision = (bp_uint32)strtoul(tok(&p), NULL, 10); u.table_chunks = (bp_uint32)strtoul(tok(&p), NULL, 10);
            af = (bp_uint32)strtoul(tok(&p), NULL, 10); ab = (bp_uint32)strtoul(tok(&p), NULL, 10);
            rc = bp_budget_ok(aead, &u, af, ab);
            printf(rc < 0 ? "ERR %d\n" : "%d\n", rc);
        } else if (!strcmp(cmd, "LIMITS")) {
            int aead = atoi(tok(&p)), kind = atoi(tok(&p)); bp_uint32 lo = 0, hi = 0; int rc = bp_frame_body_limits(aead, kind, &lo, &hi);
            if (rc) printf("ERR %d\n", rc); else printf("%lu %lu\n", (unsigned long)lo, (unsigned long)hi);
        } else printf("BAD\n");
        fflush(stdout);
    }
    return 0;
}
