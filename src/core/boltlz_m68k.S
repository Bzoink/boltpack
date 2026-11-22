/*
 * boltlz_m68k.S -- hand-written 68000 BoltLZ encoder and decoder loops
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 *
 * Optional (BOLTLZ_M68K_ASM=1).
 *
 * bl68_encode implements the BALANCED and FAST main loop of boltlz.c for
 * the BOLTLZ_M68K configuration and produces byte-identical output: same
 * hash, candidates, cost model, lazy step, backward extension and
 * end-of-match insertion. The C code still validates arguments, clears the
 * table, writes the version byte and the final literals.
 *
 * bl68_decode (at the end of this file) decodes validated sequences and
 * leaves everything else, including all error reporting, to the C decoder.
 *
 * Built for a 68000 (the default), all loads are aligned. Built with
 * -m68020 or later (__mc68020__ ... __mc68060__), longs are read and
 * compared at any address instead.
 *
 * Called from C (all m68k GCC ABIs pass arguments on the stack):
 *     void bl68_encode(bl68_state *state);
 *     void bl68_decode(bl68_dstate *state);
 * The caller guarantees room for boltlz_compress_bound(src_len) bytes, so
 * no output checks are made. Table entries hold "pointer + 1" (0 = empty);
 * BALANCED keeps the primary and secondary entry of a bucket side by side.
 */

/* C symbol names: ELF targets (Retro68, elf2x68k) have no prefix, a.out
 * targets (MiNT, AmigaOS) prefix an underscore. */
#ifdef __ELF__
#define SYM(x) x
#else
#define SYM(x) _##x
#endif

#if defined(__mc68020__) || defined(__mc68030__) || \
    defined(__mc68040__) || defined(__mc68060__)
#define BL68_020 1
#else
#define BL68_020 0
#endif

/* bl68_state fields */
#define ST_SRC     0
#define ST_END     4
#define ST_OUT     8
#define ST_TABLE  12
#define ST_LEVEL  16
#define ST_ANCHOR 20

/* Locals below the frame pointer a6 */
#define SRC   -4        /* block start */
#define ANCH  -8        /* first unencoded byte */
#define BSTEP -12       /* BALANCED skip step (word) */
#define BLEFT -10       /* misses until the next step increase (word) */
#define BSAV -16        /* encoded cost minus length of the best candidate */
#define ML   -20        /* lazy check: saved match length */
#define MD   -24        /*             saved distance */
#define MS   -28        /*             saved cost minus length */
#define E0   -32        /* repeat-distance entry (pointer + 1) or 0 */
#define FRAME -32
#define BAL_SKIP_START 0x00010080 /* BSTEP = 1, BLEFT = 128 */

/*
 * Registers in the loop:
 *   a0 end of block      a1 output          a2 hash table   a3 position P
 *   a4, a5 scratch       a6 frame
 *   d7 last distance     d5 the 4 bytes at P (big-endian)
 *   d4 table offset of P's bucket
 *   d3 best length (0: no match)            d2 best distance
 *   d0, d1, d6 scratch
 * Until FAST accepts a match, d2 is the skip step, d3 counts misses to
 * the next step increase, and d6 tracks how far P can advance safely.
 */

/* d5 = the 4 bytes at a3. */
.macro LOADV
#if BL68_020
	move.l	(%a3),%d5
#else
	move.w	%a3,%d0
	btst	#0,%d0
	bne	.Lodd\@
	move.l	(%a3),%d5
	bra	.Ldone\@
.Lodd\@:
	move.b	(%a3),%d5
	swap	%d5
	move.w	1(%a3),%d5
	lsl.l	#8,%d5
	move.b	3(%a3),%d5
.Ldone\@:
#endif
.endm

/* d4 = table offset of hash(d5): fold the halves with a rotate, multiply
 * by 0x9E37, keep the top 13 bits of the low word (as bl_hash_value). */
.macro HASH stride
	move.l	%d5,%d4
	swap	%d4
	rol.w	#5,%d4
	eor.w	%d5,%d4
	mulu.w	#0x9E37,%d4
.if \stride == 8
	and.l	#0xFFF8,%d4
.else
	lsr.w	#1,%d4
	and.w	#0x7FFC,%d4
.endif
.endm

/* Record P in its bucket; BALANCED moves the old primary to secondary. */
.macro INSERT stride
	move.l	%a3,%d0
	addq.l	#1,%d0
.if \stride == 8
	lea	(%a2,%d4.l),%a4
	move.l	(%a4),4(%a4)
	move.l	%d0,(%a4)
.else
	move.l	%d0,(%a2,%d4.w)
.endif
.endm

/* Branch to fail unless the 4 bytes at a4 equal d5. Clobbers d0. */
.macro CMPV fail
#if BL68_020
	cmp.l	(%a4),%d5
	bne	\fail
#else
	cmp.b	3(%a4),%d5
	bne	\fail
	move.w	%a4,%d0
	btst	#0,%d0
	bne	.Lodd\@
	cmp.l	(%a4),%d5
	bne	\fail
	bra	.Lok\@
.Lodd\@:
	move.b	(%a4),%d0
	swap	%d0
	move.w	1(%a4),%d0
	lsl.l	#8,%d0
	move.b	3(%a4),%d0
	cmp.l	%d0,%d5
	bne	\fail
.Lok\@:
#endif
.endm

/* d0 = match length at P (a3) against the reference a4, whose first four
 * bytes are known equal; d1 = distance. Clobbers a4, a5, d6. Counters over
 * 65535 use the swap idiom: DBcc runs out the low word, then the high word
 * is decremented. */
.macro COUNT
	lea	4(%a3),%a5
	addq.l	#4,%a4
	move.l	%a0,%d6
	sub.l	%a5,%d6
#if BL68_020
	moveq	#4,%d0
	cmp.l	%d0,%d6
	bcs	.Lbytes\@
	move.l	%d6,%d0
	lsr.l	#2,%d0
#else
	/* Long compares need both sides even: equal parity (distance even),
	 * then one byte if needed. */
	moveq	#8,%d0
	cmp.l	%d0,%d6
	bcs	.Lbytes\@
	btst	#0,%d1
	bne	.Lbytes\@
	move.w	%a5,%d0
	btst	#0,%d0
	beq	.Laligned\@
	cmpm.b	(%a4)+,(%a5)+
	bne	.Lmisb\@
	subq.l	#1,%d6
.Laligned\@:
	move.l	%d6,%d0
	lsr.l	#2,%d0
#endif
	subq.l	#1,%d0
.Llloop\@:
	cmpm.l	(%a4)+,(%a5)+
	dbne	%d0,.Llloop\@
	bne	.Lmisl\@
	swap	%d0
	subq.w	#1,%d0
	bcs	.Lltail\@
	swap	%d0
	bra	.Llloop\@
.Lmisl\@:
	/* The difference is inside the last long: find it bytewise. */
	subq.l	#4,%a4
	subq.l	#4,%a5
	moveq	#4,%d6
	bra	.Lbytes\@
.Lltail\@:
	moveq	#3,%d0
	and.l	%d0,%d6
.Lbytes\@:
	move.l	%d6,%d0
	beq	.Lend\@
	subq.l	#1,%d0
.Lbloop\@:
	cmpm.b	(%a4)+,(%a5)+
	dbne	%d0,.Lbloop\@
	bne	.Lmisb\@
	swap	%d0
	subq.w	#1,%d0
	bcs	.Lend\@
	swap	%d0
	bra	.Lbloop\@
.Lmisb\@:
	subq.l	#1,%a5
.Lend\@:
	move.l	%a5,%d0
	sub.l	%a3,%d0
.endm

/* d6 = encoded cost of a match: length d0, distance d1 (bl_match_cost). */
.macro COST
	moveq	#1,%d6
	cmp.l	%d7,%d1
	beq	.Llen\@
	addq.l	#1,%d6
	cmp.l	#128,%d1
	bcs	.Llen\@
	addq.l	#1,%d6
	cmp.l	#16384,%d1
	bcs	.Llen\@
	addq.l	#1,%d6
.Llen\@:
	cmp.l	#19,%d0
	bcs	.Ldone\@
	addq.l	#1,%d6
	cmp.l	#147,%d0
	bcs	.Ldone\@
	addq.l	#1,%d6
	cmp.l	#16403,%d0
	bcs	.Ldone\@
	addq.l	#1,%d6
.Ldone\@:
.endm

/* One BALANCED candidate, entry (pointer + 1) in d0. Skips to next;
 * jumps to out when the best match reaches the end of the block. */
.macro CAND idx, next, out
.if \idx != 0
	tst.l	%d0
	beq	\next
	cmp.l	E0(%a6),%d0
	beq	\next
.endif
	subq.l	#1,%d0
	move.l	%d0,%a4
	CMPV	\next
	move.l	%a3,%d1
	sub.l	%a4,%d1
	COUNT
	COST
	sub.l	%d0,%d6		/* a borrow means the match saves bytes */
	bcc	\next
	cmp.l	BSAV(%a6),%d6
	blt	.Ltake\@
	bne	\next
	cmp.l	%d3,%d0
	bls	\next
.Ltake\@:
	move.l	%d0,%d3
	move.l	%d1,%d2
	move.l	%d6,BSAV(%a6)
	cmpa.l	%a0,%a5		/* COUNT leaves a5 at the first mismatch or end */
	beq	\out
.endm

/* Encode literals ANCH..P and the match d3/d2; P = a3 is the match start. */
.macro EMIT
	move.l	%a3,%d1
	sub.l	ANCH(%a6),%d1
	moveq	#7,%d0
	cmp.l	%d0,%d1
	bcc	.Ll7\@
	move.l	%d1,%d0
.Ll7\@:
	lsl.b	#4,%d0
	moveq	#19,%d6
	cmp.l	%d6,%d3
	bcc	.Lm15\@
	move.b	%d3,%d6
	subq.b	#4,%d6
	or.b	%d6,%d0
	bra	.Lrep\@
.Lm15\@:
	or.b	#15,%d0
.Lrep\@:
	cmp.l	%d7,%d2
	bne	.Ltag\@
	or.b	#0x80,%d0
.Ltag\@:
	move.b	%d0,(%a1)+
	moveq	#7,%d0
	cmp.l	%d0,%d1
	bcs	.Lcopy\@
	move.l	%d1,%d0
	subq.l	#7,%d0
	bsr	.Lwvar
.Lcopy\@:
	move.l	ANCH(%a6),%a4
	bsr	.Lcopy
	moveq	#19,%d0
	cmp.l	%d0,%d3
	bcs	.Ldist\@
	move.l	%d3,%d0
	moveq	#19,%d6
	sub.l	%d6,%d0
	bsr	.Lwvar
.Ldist\@:
	cmp.l	%d7,%d2
	beq	.Lend\@
	move.l	%d2,%d0
	bsr	.Lwvar
.Lend\@:
	move.l	%d2,%d7
.endm

/* Insert position a3 if at least four bytes remain there. */
.macro TAILINSERT stride
	move.l	%a0,%d0
	sub.l	%a3,%d0
	moveq	#4,%d1
	cmp.l	%d1,%d0
	bcs	.Lskip\@
	LOADV
	HASH	\stride
	INSERT	\stride
.Lskip\@:
.endm

/* FAST search at P: replace the bucket's only entry and use the old one
 * as the candidate (bl_fast_find). d2 and d3 still hold the skip state on
 * a miss, and d6 is the number of bytes P can advance and still read four. */
.macro FIND_FAST miss
	move.l	(%a2,%d4.w),%d0
	lea	1(%a3),%a4
	move.l	%a4,(%a2,%d4.w)
	/* Subtracting one also makes an empty entry fail the unsigned test. */
	subq.l	#1,%d0
	move.l	%d0,%a4
	cmpa.l	%a3,%a4
	bcc	\miss
	CMPV	\miss
	move.l	%a3,%d1
	sub.l	%a4,%d1
	COUNT
	/* Only a four-byte match with a three-byte explicit distance can
	 * cost as many bytes as it replaces. */
	cmpi.l	#4,%d0
	bne	.Ltake\@
	cmpi.l	#16384,%d1
	bcs	.Ltake\@
	cmp.l	%d7,%d1
	beq	.Ltake\@
	/* COUNT uses d6, so restore the limit when rejecting this match. */
	move.l	%a0,%d6
	sub.l	%a3,%d6
	subq.l	#4,%d6
	bra	\miss
.Ltake\@:
	move.l	%d0,%d3
	move.l	%d1,%d2
.endm

/* The main loop. stride 8 = BALANCED (lazy step), 4 = FAST. */
.macro ENCODE stride
.if \stride == 4
.Lreset\@:
	moveq	#1,%d2
	moveq	#31,%d3
	move.l	%a0,%d6
	sub.l	%a3,%d6
	subq.l	#4,%d6
	bcs	.Lexit
.else
	move.l	#BAL_SKIP_START,BSTEP(%a6)
	bra	.Lcheck\@
.endif
.Ltop\@:
	LOADV
	HASH	\stride
.if \stride == 4
	FIND_FAST .Lmiss\@
.else
	bsr	.Lfind_bal
.Lfound\@:
	INSERT	\stride
	tst.l	%d3
	bne	.Lmatch\@
	/* Step starts at 1 and grows every 128 misses, up to 16. */
	moveq	#0,%d0
	move.w	BSTEP(%a6),%d0
	move.l	%a0,%d1
	sub.l	%a3,%d1
	subq.l	#4,%d1
	cmp.l	%d1,%d0
	bhi	.Lexit
	adda.w	%d0,%a3
	subq.w	#1,BLEFT(%a6)
	bne	.Ltop\@
	move.w	#128,BLEFT(%a6)
	cmpi.w	#16,%d0
	beq	.Ltop\@
	addq.w	#1,BSTEP(%a6)
	bra	.Ltop\@
.Lmatch\@:
	/* Lazy step: a match at P+1 saving two more bytes wins. Its search
	 * result is reused as the search at the new position. */
	moveq	#32,%d0
	cmp.l	%d0,%d3
	bcc	.Lnolazy\@
	move.l	%a0,%d0
	sub.l	%a3,%d0
	moveq	#5,%d1
	cmp.l	%d1,%d0
	bcs	.Lnolazy\@
	move.l	%d3,ML(%a6)
	move.l	%d2,MD(%a6)
	move.l	BSAV(%a6),MS(%a6)
	addq.l	#1,%a3
	LOADV
	HASH	\stride
	bsr	.Lfind_bal
	move.l	MS(%a6),%d0
	subq.l	#1,%d0
	cmp.l	BSAV(%a6),%d0
	bgt	.Lfound\@
	subq.l	#1,%a3
	move.l	ML(%a6),%d3
	move.l	MD(%a6),%d2
.Lnolazy\@:
.endif
	/* Extend the match backwards over pending literals. */
.Lbw\@:
	cmpa.l	ANCH(%a6),%a3
	bls	.Lbwdone\@
	move.l	%a3,%d0
	sub.l	SRC(%a6),%d0
	cmp.l	%d2,%d0
	bls	.Lbwdone\@
	move.l	%a3,%a4
	suba.l	%d2,%a4
	move.b	-1(%a3),%d0
	cmp.b	-1(%a4),%d0
	bne	.Lbwdone\@
	subq.l	#1,%a3
	addq.l	#1,%d3
	bra	.Lbw\@
.Lbwdone\@:
	EMIT
	adda.l	%d3,%a3
	move.l	%a3,ANCH(%a6)
.if \stride == 8
	move.l	#BAL_SKIP_START,BSTEP(%a6)
.endif
	/* Keep a little history at the end of the match. */
	subq.l	#2,%a3
	TAILINSERT \stride
	addq.l	#1,%a3
	TAILINSERT \stride
	addq.l	#1,%a3
.if \stride == 4
	bra	.Lreset\@
.Lmiss\@:
	/* Step starts at 1 and grows every 32 misses, up to 16. */
	sub.l	%d2,%d6
	bcs	.Lexit
	adda.w	%d2,%a3
	dbra	%d3,.Ltop\@
	moveq	#31,%d3
	cmpi.w	#16,%d2
	beq	.Ltop\@
	addq.w	#1,%d2
	bra	.Ltop\@
.else
.Lcheck\@:
	move.l	%a0,%d0
	sub.l	%a3,%d0
	moveq	#4,%d1
	cmp.l	%d1,%d0
	bcc	.Ltop\@
.endif
.endm

	.text
	.even
	.globl	SYM(bl68_encode)
SYM(bl68_encode):
	link	%a6,#FRAME
	movem.l	%d2-%d7/%a2-%a5,-(%sp)
	move.l	8(%a6),%a4
	move.l	ST_SRC(%a4),%a3
	move.l	%a3,SRC(%a6)
	move.l	%a3,ANCH(%a6)
	move.l	ST_END(%a4),%a0
	move.l	ST_OUT(%a4),%a1
	move.l	ST_TABLE(%a4),%a2
	moveq	#0,%d7
	moveq	#1,%d0
	cmp.l	ST_LEVEL(%a4),%d0
	beq	.Lfast
	ENCODE	8
	bra	.Lexit
.Lfast:
	ENCODE	4
.Lexit:
	move.l	8(%a6),%a4
	move.l	%a1,ST_OUT(%a4)
	move.l	ANCH(%a6),ST_ANCHOR(%a4)
	movem.l	(%sp)+,%d2-%d7/%a2-%a5
	unlk	%a6
	rts

/* BALANCED search at P: the repeat distance, then the bucket's primary and
 * secondary entries (bl_find). d7 comes from an earlier match and each
 * nonempty table entry was inserted strictly before P. Out: d3/d2 best,
 * BSAV its encoded cost minus length. */
.Lfind_bal:
	moveq	#0,%d3
	clr.l	BSAV(%a6)
	moveq	#0,%d0
	tst.l	%d7
	beq	.Lfb_e0
	move.l	%a3,%d0
	sub.l	%d7,%d0
	addq.l	#1,%d0
.Lfb_e0:
	move.l	%d0,E0(%a6)
	beq	.Lfb_c1
	CAND	0, .Lfb_c1, .Lfb_end
.Lfb_c1:
	move.l	(%a2,%d4.l),%d0
	CAND	1, .Lfb_c2, .Lfb_end
.Lfb_c2:
	move.l	4(%a2,%d4.l),%d0
	CAND	2, .Lfb_end, .Lfb_end
.Lfb_end:
	rts

/* Write d0 as LEB128 at a1. Clobbers d0, d6. */
.Lwvar:
	cmp.l	#128,%d0
	bcs	.Lwv_last
.Lwv_loop:
	move.b	%d0,%d6
	or.b	#0x80,%d6
	move.b	%d6,(%a1)+
	lsr.l	#7,%d0
	cmp.l	#128,%d0
	bcc	.Lwv_loop
.Lwv_last:
	move.b	%d0,(%a1)+
	rts

/* Copy d1 bytes from a4 to a1 (disjoint). Clobbers d0, d6, a4. */
.Lcopy:
	move.l	%d1,%d0
	beq	.Lcp_end
	moveq	#16,%d6
	cmp.l	%d6,%d0
	bcs	.Lcp_bytes
#if !BL68_020
	/* Long moves need both pointers even. */
	move.l	%a4,%d6
	sub.l	%a1,%d6
	btst	#0,%d6
	bne	.Lcp_bytes
	move.w	%a1,%d6
	btst	#0,%d6
	beq	.Lcp_al
	move.b	(%a4)+,(%a1)+
	subq.l	#1,%d0
.Lcp_al:
#endif
	move.l	%d0,%d6
	lsr.l	#2,%d6
	subq.l	#1,%d6
.Lcp_l:
	move.l	(%a4)+,(%a1)+
	dbra	%d6,.Lcp_l
	swap	%d6
	subq.w	#1,%d6
	bcs	.Lcp_lt
	swap	%d6
	bra	.Lcp_l
.Lcp_lt:
	moveq	#3,%d6
	and.l	%d6,%d0
	beq	.Lcp_end
.Lcp_bytes:
	subq.l	#1,%d0
.Lcp_b:
	move.b	(%a4)+,(%a1)+
	dbra	%d0,.Lcp_b
	swap	%d0
	subq.w	#1,%d0
	bcs	.Lcp_end
	swap	%d0
	bra	.Lcp_b
.Lcp_end:
	rts

/*
 * Decoder fast loop.
 *
 *     void bl68_decode(bl68_dstate *state);
 *
 * Decodes whole sequences while at least ten input bytes remain, writing
 * nothing until every field of a sequence is validated: canonical LEB128
 * fields, literal and match bounds, distance within the output. Anything
 * else (a malformed field, the end of the stream, the final literals) stops
 * the loop at the start of that sequence, and the C decoder continues from
 * there and reports any error exactly as before.
 */

/* bl68_dstate fields */
#define DS_IN      0
#define DS_SRCEND  4
#define DS_OUT     8
#define DS_DSTEND 12
#define DS_DST    16
#define DS_LAST   20

/*
 * Registers:
 *   a0 input        a1 output       a2 end of input   a3 end of output
 *   a4 match source a5 match fields a6 start of output
 *   d2 tag   d3 literal length   d4 match length   d5 distance
 *   d6 start of the current sequence   d7 last distance   d0, d1 scratch
 */

/* d0 = canonical LEB128 at \p, accepting exactly what bl_read_var accepts
 * (at most three bytes, no zero final byte after the first, third byte 1 to
 * 16); otherwise stop at the start of the sequence. */
.macro DVAR p
	moveq	#0,%d0
	move.b	(\p)+,%d0
	bpl	.Ldone\@
	and.b	#0x7F,%d0
	moveq	#0,%d1
	move.b	(\p)+,%d1
	bmi	.Lthree\@
	beq	.Ld_bail
	lsl.l	#7,%d1
	or.l	%d1,%d0
	bra	.Ldone\@
.Lthree\@:
	and.b	#0x7F,%d1
	lsl.l	#7,%d1
	or.l	%d1,%d0
	moveq	#0,%d1
	move.b	(\p)+,%d1
	beq	.Ld_bail
	cmp.b	#16,%d1
	bhi	.Ld_bail
	swap	%d1
	lsr.l	#2,%d1
	or.l	%d1,%d0
.Ldone\@:
.endm

/* Copy d0 (at most 32) bytes forward from \src to a1 through an unrolled
 * MOVE.B run; byte order makes it correct for overlapping matches. */
.macro SHORTCOPY src, done
	add.w	%d0,%d0
	neg.w	%d0
	jmp	(.Ltab\@,%pc,%d0.w)
	.rept	32
	move.b	(\src)+,(%a1)+
	.endr
.Ltab\@:
	bra	\done
.endm

	.globl	SYM(bl68_decode)
SYM(bl68_decode):
	movem.l	%d2-%d7/%a2-%a6,-(%sp)
	move.l	48(%sp),%a4
	move.l	DS_IN(%a4),%a0
	move.l	DS_SRCEND(%a4),%a2
	move.l	DS_OUT(%a4),%a1
	move.l	DS_DSTEND(%a4),%a3
	move.l	DS_DST(%a4),%a6
	move.l	DS_LAST(%a4),%d7
.Ld_loop:
	move.l	%a2,%d0
	sub.l	%a0,%d0
	moveq	#10,%d1
	cmp.l	%d1,%d0
	bcs	.Ld_exit
	move.l	%a0,%d6
	moveq	#0,%d2
	move.b	(%a0)+,%d2
	/* literal length */
	moveq	#0,%d3
	move.b	%d2,%d3
	lsr.b	#4,%d3
	and.b	#7,%d3
	cmp.b	#7,%d3
	bne	.Ld_lit
	DVAR	%a0
	add.l	%d0,%d3
.Ld_lit:
	/* literals plus six bytes of match fields must be in the input,
	 * which also rules out the final literal-only sequence */
	move.l	%a2,%d0
	sub.l	%a0,%d0
	subq.l	#6,%d0
	bcs	.Ld_bail
	cmp.l	%d3,%d0
	bcs	.Ld_bail
	move.l	%a3,%d0
	sub.l	%a1,%d0
	cmp.l	%d3,%d0
	bcs	.Ld_bail
	lea	(%a0,%d3.l),%a5
	/* match length */
	moveq	#15,%d4
	and.b	%d2,%d4
	addq.l	#4,%d4
	cmp.b	#19,%d4
	bne	.Ld_len
	DVAR	%a5
	add.l	%d0,%d4
.Ld_len:
	/* distance: repeat or explicit */
	move.l	%d7,%d5
	tst.b	%d2
	bmi	.Ld_dist
	DVAR	%a5
	move.l	%d0,%d5
.Ld_dist:
	tst.l	%d5
	beq	.Ld_bail
	move.l	%a1,%d0
	sub.l	%a6,%d0
	add.l	%d3,%d0
	cmp.l	%d5,%d0
	bcs	.Ld_bail
	move.l	%a3,%d0
	sub.l	%a1,%d0
	sub.l	%d3,%d0
	cmp.l	%d4,%d0
	bcs	.Ld_bail
	move.l	%d5,%d7

	/* literals: a0 -> a1 */
	move.l	%d3,%d0
	moveq	#32,%d1
	cmp.l	%d1,%d0
	bhi	.Ld_litlong
.Ld_litshort:
	SHORTCOPY %a0, .Ld_match
.Ld_litlong:
#if !BL68_020
	move.l	%a0,%d1
	sub.l	%a1,%d1
	btst	#0,%d1
	bne	.Ld_litbytes
	move.w	%a1,%d1
	btst	#0,%d1
	beq	.Ld_lital
	move.b	(%a0)+,(%a1)+
	subq.l	#1,%d0
.Ld_lital:
#endif
	move.l	%d0,%d1
	lsr.l	#4,%d1
	subq.l	#1,%d1
.Ld_lit16:
	move.l	(%a0)+,(%a1)+
	move.l	(%a0)+,(%a1)+
	move.l	(%a0)+,(%a1)+
	move.l	(%a0)+,(%a1)+
	dbra	%d1,.Ld_lit16
	swap	%d1
	subq.w	#1,%d1
	bcs	.Ld_litrem
	swap	%d1
	bra	.Ld_lit16
.Ld_litrem:
	moveq	#15,%d1
	and.l	%d1,%d0
	bra	.Ld_litshort
#if !BL68_020
.Ld_litbytes:
	subq.l	#1,%d0
.Ld_litb:
	move.b	(%a0)+,(%a1)+
	dbra	%d0,.Ld_litb
	swap	%d0
	subq.w	#1,%d0
	bcs	.Ld_match
	swap	%d0
	bra	.Ld_litb
#endif

	/* match: a1 - distance -> a1, forward */
.Ld_match:
	move.l	%a5,%a0
	move.l	%a1,%a4
	suba.l	%d5,%a4
	move.l	%d4,%d0
	moveq	#32,%d1
	cmp.l	%d1,%d0
	bhi	.Ld_mlong
.Ld_mshort:
	SHORTCOPY %a4, .Ld_loop
.Ld_mlong:
	moveq	#1,%d1
	cmp.l	%d1,%d5
	beq	.Ld_mfill
	moveq	#4,%d1
	cmp.l	%d1,%d5
	bcs	.Ld_mdouble
#if !BL68_020
	/* Longs need both sides even: an even distance, then align. */
	btst	#0,%d5
	bne	.Ld_mdouble
.Ld_malign:
	move.w	%a1,%d1
	btst	#0,%d1
	beq	.Ld_mal
	move.b	(%a4)+,(%a1)+
	subq.l	#1,%d0
#endif
.Ld_mal:
	/* distance >= 4: each long reads bytes already written */
	move.l	%d0,%d1
	lsr.l	#4,%d1
	subq.l	#1,%d1
.Ld_m16:
	move.l	(%a4)+,(%a1)+
	move.l	(%a4)+,(%a1)+
	move.l	(%a4)+,(%a1)+
	move.l	(%a4)+,(%a1)+
	dbra	%d1,.Ld_m16
	swap	%d1
	subq.w	#1,%d1
	bcs	.Ld_mrem
	swap	%d1
	bra	.Ld_m16
.Ld_mrem:
	moveq	#15,%d1
	and.l	%d1,%d0
	bra	.Ld_mshort
.Ld_mdouble:
	/* Distance 2 or 3, or odd on a 68000: once the first distance bytes
	 * are written, the output repeats with twice the distance, which is
	 * even and at least 4, so the rest can be copied as longs. */
	move.l	%d5,%d1
	moveq	#32,%d2
	add.l	%d2,%d1
	cmp.l	%d1,%d0
	bcs	.Ld_mbytes
	sub.l	%d5,%d0
	move.l	%d5,%d1
	subq.l	#1,%d1
.Ld_md:
	move.b	(%a4)+,(%a1)+
	dbra	%d1,.Ld_md
	swap	%d1
	subq.w	#1,%d1
	bcs	.Ld_mdd
	swap	%d1
	bra	.Ld_md
.Ld_mdd:
	suba.l	%d5,%a4
#if !BL68_020
	bra	.Ld_malign
#else
	bra	.Ld_mal
#endif
.Ld_mfill:
	/* distance 1: a run of one byte */
	move.b	(%a4),%d1
#if !BL68_020
	move.w	%a1,%d2
	btst	#0,%d2
	beq	.Ld_fal
	move.b	%d1,(%a1)+
	subq.l	#1,%d0
.Ld_fal:
#endif
	move.b	%d1,%d2
	lsl.w	#8,%d2
	move.b	%d1,%d2
	move.w	%d2,%d1
	swap	%d1
	move.w	%d2,%d1
	move.l	%d0,%d2
	lsr.l	#4,%d2
	subq.l	#1,%d2
.Ld_f16:
	move.l	%d1,(%a1)+
	move.l	%d1,(%a1)+
	move.l	%d1,(%a1)+
	move.l	%d1,(%a1)+
	dbra	%d2,.Ld_f16
	swap	%d2
	subq.w	#1,%d2
	bcs	.Ld_frem
	swap	%d2
	bra	.Ld_f16
.Ld_frem:
	moveq	#15,%d1
	and.l	%d1,%d0
	lea	-1(%a1),%a4
	bra	.Ld_mshort
.Ld_mbytes:
	subq.l	#1,%d0
.Ld_mb:
	move.b	(%a4)+,(%a1)+
	dbra	%d0,.Ld_mb
	swap	%d0
	subq.w	#1,%d0
	bcs	.Ld_loop
	swap	%d0
	bra	.Ld_mb

.Ld_bail:
	move.l	%d6,%a0
.Ld_exit:
	move.l	48(%sp),%a4
	move.l	%a0,DS_IN(%a4)
	move.l	%a1,DS_OUT(%a4)
	move.l	%d7,DS_LAST(%a4)
	movem.l	(%sp)+,%d2-%d7/%a2-%a6
	rts
