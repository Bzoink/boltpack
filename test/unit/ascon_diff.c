/*
 * ascon_diff.c -- differential test of the strict C89 Ascon (bp_ascon_c89.c) against
 *   - ascon-c's own reference AEAD (vendor/ascon-c/ref/aead.c: crypto_aead_encrypt and
 *     crypto_aead_decrypt), and
 *   - bp_ascon_ref.c (our code on the reference permutation), for the XOF and the AEAD,
 * and, for Ascon-CXOF128, ascon-c's reference crypto_cxof (vendor/ascon-c/ref_cxof/hash.c),
 * on random inputs of every length around the 8- and 16-byte rates, long inputs,
 * chunked absorbs, split squeezes and tampered frames.
 *
 * The reference code is linked under other names: bp_ascon_ref.c is compiled with
 * -Dbp_xof_init=ref_xof_init and the like.  usage: ascon_diff ITERATIONS [SEED]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bp_ascon.h"

void ref_aead_seal(const bp_uint8 key[16], const bp_uint8 nonce[16], const bp_uint8 *ad, bp_uint32 adlen,
                   const bp_uint8 *pt, bp_uint32 ptlen, bp_uint8 *ct, bp_uint8 tag[16]);
int ref_aead_open(const bp_uint8 key[16], const bp_uint8 nonce[16], const bp_uint8 *ad, bp_uint32 adlen,
                  const bp_uint8 *ct, bp_uint32 ctlen, const bp_uint8 tag[16], bp_uint8 *pt);
void ref_xof_init(bp_xof *x);
void ref_xof_absorb(bp_xof *x, const bp_uint8 *d, bp_uint32 n);
void ref_xof_squeeze(bp_xof *x, bp_uint8 *o, bp_uint32 n);
int crypto_aead_encrypt(unsigned char *c, unsigned long long *clen, const unsigned char *m, unsigned long long mlen,
                        const unsigned char *ad, unsigned long long adlen, const unsigned char *nsec,
                        const unsigned char *npub, const unsigned char *k);
int crypto_cxof(unsigned char *out, unsigned long long outlen, const unsigned char *in, unsigned long long inlen,
                const unsigned char *cs, unsigned long long cslen);
int crypto_aead_decrypt(unsigned char *m, unsigned long long *mlen, unsigned char *nsec, const unsigned char *c,
                        unsigned long long clen, const unsigned char *ad, unsigned long long adlen,
                        const unsigned char *npub, const unsigned char *k);

static unsigned long long rs;
static unsigned long long rnd(void)
{
    rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27;
    return rs * 2685821657736338717ULL;
}
static unsigned rn(unsigned n) { return (unsigned)(rnd() % n); }
static void fill(unsigned char *p, unsigned n) { unsigned i; for (i = 0; i < n; i++) p[i] = (unsigned char)rnd(); }
static unsigned pick_len(void)
{
    unsigned r = rn(100);
    return r < 60 ? rn(41) : r < 90 ? rn(81) : r < 98 ? rn(400) : rn(3000);
}

static unsigned long fails, total;
#define CHECK(c, what) do { total++; if (!(c)) { if (fails++ < 20) printf("FAIL iter %lu: %s\n", it, what); } } while (0)

int main(int argc, char **argv)
{
    unsigned long n = argc > 1 ? strtoul(argv[1], NULL, 10) : 1000, it;
    static unsigned char key[16], nonce[16], ad[4000], pt[4000], ct[4100], ct_ref[4100], tag[16], tag_ref[16], out[4100], out2[4100];
    static unsigned char blk[300], o1[300], o2[300];
    rs = argc > 2 ? strtoull(argv[2], NULL, 10) | 1 : 88172645463325252ULL;
    for (it = 0; it < n; it++) {
        unsigned al = pick_len(), ml = pick_len(), i;
        unsigned long long clen = 0, mlen2 = 0;
        int rc;
        fill(key, 16); fill(nonce, 16); fill(ad, al); fill(pt, ml);

        /* encryption: C89 port, ascon-c ref/aead.c, and our reference-permutation backend */
        bp_ascon_aead_seal(key, nonce, ad, al, pt, ml, ct, tag);
        crypto_aead_encrypt(ct_ref, &clen, pt, ml, ad, al, NULL, nonce, key);
        CHECK(clen == (unsigned long long)ml + 16, "ref clen");
        CHECK(memcmp(ct, ct_ref, ml) == 0 && memcmp(tag, ct_ref + ml, 16) == 0, "seal vs ascon-c ref/aead.c");
        ref_aead_seal(key, nonce, ad, al, pt, ml, out, tag_ref);
        CHECK(memcmp(ct, out, ml) == 0 && memcmp(tag, tag_ref, 16) == 0, "seal vs bp_ascon_ref");

        /* decryption of a good frame, by the port and by the reference */
        memset(out, 0xEE, ml);
        rc = bp_ascon_aead_open(key, nonce, ad, al, ct, ml, tag, out);
        CHECK(rc == 0 && memcmp(out, pt, ml) == 0, "open good (port)");
        rc = crypto_aead_decrypt(out2, &mlen2, NULL, ct_ref, clen, ad, al, nonce, key);
        CHECK(rc == 0 && mlen2 == ml && memcmp(out2, pt, ml) == 0, "open good (ref)");

        /* a tampered frame: the port and the reference both refuse; the port wipes its output */
        {
            unsigned char t2[16];
            unsigned where = rn(5);
            unsigned char *k2 = key, *n2 = nonce, *a2 = ad, *c2 = ct, *tg = tag;
            unsigned char kcopy[16], ncopy[16];
            unsigned char *acopy = (unsigned char *)malloc(al + 1), *ccopy = (unsigned char *)malloc(ml + 1);
            memcpy(kcopy, key, 16); memcpy(ncopy, nonce, 16); memcpy(acopy, ad, al); memcpy(ccopy, ct, ml); memcpy(t2, tag, 16);
            k2 = kcopy; n2 = ncopy; a2 = acopy; c2 = ccopy; tg = t2;
            if (where == 0) k2[rn(16)] ^= (unsigned char)(1u << rn(8));
            else if (where == 1) n2[rn(16)] ^= (unsigned char)(1u << rn(8));
            else if (where == 2 && al) a2[rn(al)] ^= (unsigned char)(1u << rn(8));
            else if (where == 3 && ml) c2[rn(ml)] ^= (unsigned char)(1u << rn(8));
            else tg[rn(16)] ^= (unsigned char)(1u << rn(8));
            memset(out, 0xEE, ml);
            rc = bp_ascon_aead_open(k2, n2, a2, al, c2, ml, tg, out);
            CHECK(rc == -1, "open tampered (port) refuses");
            { int z = 1; for (i = 0; i < ml; i++) if (out[i]) z = 0; CHECK(z, "open tampered wipes output"); }
            memcpy(ct_ref + 0, c2, ml); memcpy(ct_ref + ml, tg, 16);
            rc = crypto_aead_decrypt(out2, &mlen2, NULL, ct_ref, (unsigned long long)ml + 16, a2, al, n2, k2);
            CHECK(rc != 0, "open tampered (ref) refuses");
            free(acopy); free(ccopy);
        }

        /* CXOF128: random customisation string, chunked absorb, split squeeze, against ascon-c's reference */
        {
            bp_xof c;
            unsigned zl = (rn(10) < 8) ? rn(40) : rn(257), ml = pick_len() % 300, ol = rn(150), off = 0, got = 0;
            fill(blk, ml); fill(ad, zl);
            CHECK(bp_cxof_init(&c, ad, zl) == 0, "cxof init");
            while (off < ml) {
                unsigned t = 1 + rn(20); if (t > ml - off) t = ml - off;
                bp_xof_absorb(&c, blk + off, t); off += t;
            }
            while (got < ol) {
                unsigned t = rn(40); if (t > ol - got) t = ol - got;
                bp_xof_squeeze(&c, o1 + got, t); got += t;
            }
            crypto_cxof(o2, ol, blk, ml, ad, zl);
            CHECK(memcmp(o1, o2, ol) == 0, "cxof vs ascon-c reference");
        }

        /* XOF: random absorb splits and squeeze splits against the reference-permutation XOF */
        {
            bp_xof a, b;
            unsigned dl = pick_len() % 400, ol = rn(200), off = 0, got = 0;
            fill(blk, dl);
            bp_xof_init(&a); ref_xof_init(&b);
            while (off < dl) {
                unsigned t = 1 + rn(20); if (t > dl - off) t = dl - off;
                bp_xof_absorb(&a, blk + off, t); off += t;
            }
            ref_xof_absorb(&b, blk, dl);
            while (got < ol) {
                unsigned t = rn(40); if (t > ol - got) t = ol - got;
                bp_xof_squeeze(&a, o1 + got, t); got += t;
            }
            ref_xof_squeeze(&b, o2, ol);
            CHECK(memcmp(o1, o2, ol) == 0, "xof vs bp_ascon_ref");
        }
    }
    printf("%lu iterations, %lu checks, %lu failed\n", n, total, fails);
    return fails ? 1 : 0;
}
