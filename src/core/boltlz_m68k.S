/*
 * boltlz_m68k.S -- hand-written 68000 BoltLZ encoder main loop
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 *
 * Optional (BOLTLZ_M68K_ASM=1). Implements the BALANCED and FAST main loop
 * of boltlz.c for the BOLTLZ_M68K configuration and produces byte-identical
 * output: same hash, candidates, cost model, lazy step, backward extension
 * and end-of-match insertion. The C code still validates arguments, clears
 * the table, writes the version byte and the final literals.
 *
 * Built for a 68000 (the default), all loads are aligned. Built with
 * -m68020 or later (__mc68020__ ... __mc68060__), longs are read and
 * compared at any address instead.
 *
 * Called from C (all m68k GCC ABIs pass arguments on the stack):
 *     void bl68_encode(bl68_state *state);
 * The caller guarantees room for boltlz_compress_bound(src_len) bytes, so
 * no output checks are made. Table entries hold "pointer + 1" (0 = empty);
 * BALANCED keeps the primary and secondary entry of a bucket side by side.
 */

#define BL_CAT(a, b) a##b
#define BL_XCAT(a, b) BL_CAT(a, b)
#define SYM(x) BL_XCAT(__USER_LABEL_PREFIX__, x)

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
#define MISS -12        /* misses since the last match */
#define BSAV -16        /* saving of the best candidate */
#define ML   -20        /* lazy check: saved match length */
#define MD   -24        /*             saved distance */
#define MS   -28        /*             saved saving */
#define E0   -32        /* repeat-distance entry (pointer + 1) or 0 */
#define FRAME -32

/*
 * Registers in the loop:
 *   a0 end of block      a1 output          a2 hash table   a3 position P
 *   a4, a5 scratch       a6 frame
 *   d7 last distance     d5 the 4 bytes at P (big-endian)
 *   d4 table offset of P's bucket
 *   d3 best length (0: no match)            d2 best distance
 *   d0, d1, d6 scratch
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
	and.l	#0x7FFC,%d4
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
	move.l	%d0,(%a2,%d4.l)
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
	cmpa.l	%a3,%a4
	bcc	\next
	CMPV	\next
	move.l	%a3,%d1
	sub.l	%a4,%d1
	COUNT
	COST
	cmp.l	%d6,%d0
	bls	\next
	sub.l	%d0,%d6
	neg.l	%d6
	cmp.l	BSAV(%a6),%d6
	bhi	.Ltake\@
	bne	\next
	cmp.l	%d3,%d0
	bls	\next
.Ltake\@:
	move.l	%d0,%d3
	move.l	%d1,%d2
	move.l	%d6,BSAV(%a6)
	move.l	%a0,%d6
	sub.l	%a3,%d6
	cmp.l	%d3,%d6
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

/* The main loop. stride 8 = BALANCED (lazy step), 4 = FAST. */
.macro ENCODE stride, find, shift
	bra	.Lcheck\@
.Ltop\@:
	LOADV
	HASH	\stride
	bsr	\find
.Lfound\@:
	INSERT	\stride
	tst.l	%d3
	bne	.Lmatch\@
	/* No match: step 1 + misses >> shift, at most 16 and the rest. */
	move.l	MISS(%a6),%d0
	lsr.l	#\shift,%d0
	addq.l	#1,%d0
	moveq	#16,%d1
	cmp.l	%d1,%d0
	bls	.Ls1\@
	move.l	%d1,%d0
.Ls1\@:
	move.l	%a0,%d1
	sub.l	%a3,%d1
	cmp.l	%d1,%d0
	bls	.Ls2\@
	move.l	%d1,%d0
.Ls2\@:
	adda.l	%d0,%a3
	cmpi.l	#2048,MISS(%a6)
	bcc	.Lcheck\@
	addq.l	#1,MISS(%a6)
	bra	.Lcheck\@
.Lmatch\@:
.if \stride == 8
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
	bsr	\find
	move.l	MS(%a6),%d0
	addq.l	#1,%d0
	cmp.l	BSAV(%a6),%d0
	bcs	.Lfound\@
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
	clr.l	MISS(%a6)
	/* Keep a little history at the end of the match. */
	subq.l	#2,%a3
	TAILINSERT \stride
	addq.l	#1,%a3
	TAILINSERT \stride
	addq.l	#1,%a3
.Lcheck\@:
	move.l	%a0,%d0
	sub.l	%a3,%d0
	moveq	#4,%d1
	cmp.l	%d1,%d0
	bcc	.Ltop\@
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
	clr.l	MISS(%a6)
	moveq	#1,%d0
	cmp.l	ST_LEVEL(%a4),%d0
	beq	.Lfast
	ENCODE	8, .Lfind_bal, 7
	bra	.Lexit
.Lfast:
	ENCODE	4, .Lfind_fast, 5
.Lexit:
	move.l	8(%a6),%a4
	move.l	%a1,ST_OUT(%a4)
	move.l	ANCH(%a6),ST_ANCHOR(%a4)
	movem.l	(%sp)+,%d2-%d7/%a2-%a5
	unlk	%a6
	rts

/* BALANCED search at P: the repeat distance, then the bucket's primary and
 * secondary entries (bl_find). Out: d3/d2 best, BSAV its saving. */
.Lfind_bal:
	moveq	#0,%d3
	moveq	#0,%d2
	clr.l	BSAV(%a6)
	moveq	#0,%d0
	tst.l	%d7
	beq	.Lfb_e0
	move.l	%a3,%d1
	sub.l	SRC(%a6),%d1
	cmp.l	%d7,%d1
	bcs	.Lfb_e0
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

/* FAST search at P: the bucket's only entry (bl_fast_find). */
.Lfind_fast:
	moveq	#0,%d3
	move.l	(%a2,%d4.l),%d0
	beq	.Lff_end
	subq.l	#1,%d0
	move.l	%d0,%a4
	cmpa.l	%a3,%a4
	bcc	.Lff_end
	CMPV	.Lff_end
	move.l	%a3,%d1
	sub.l	%a4,%d1
	COUNT
	COST
	cmp.l	%d6,%d0
	bls	.Lff_end
	move.l	%d0,%d3
	move.l	%d1,%d2
.Lff_end:
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
