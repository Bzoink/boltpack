/*
 * crc32_m68k.S -- hand-written CRC-32 for the Motorola 68000 family
 * Copyright (C) 2026 Tony Mattsson
 * Licensed under the BSD 2-Clause License
 *
 * Optional (BP_CRC32_M68K_ASM, set together with BOLTLZ_M68K_ASM=1).
 *
 *     bp_uint32 bp_crc32_m68k(bp_uint32 c, const bp_uint8 *data,
 *                             bp_uint32 len, const bp_uint32 *tables);
 *
 * Folds len bytes into the raw CRC-32 state c (IEEE 802.3, reflected; the
 * caller does the initial and final inversion) and returns the new state,
 * exactly as the byte-at-a-time loop in compress.c does. len must be at
 * least 1; compress.c only calls this for 16 bytes or more.
 *
 * tables points at 5 * 256 longs built by crc32_build_table():
 *     T0 (the standard table), then S0, S1, S2, S3, where Sk is table Tk of
 * "slicing-by-4" with every entry byte-swapped, so that the loop can keep
 * the CRC byte-swapped ("r") and fold in a big-endian long straight from
 * memory:
 *     r = r ^ long(data);   store r;   byte i of r (memory order) selects
 *     r' = S3[b0] ^ S2[b1] ^ S1[b2] ^ S0[b3]
 * Four bytes per round instead of one, no shift by 8 on the 68000, which
 * costs 22 cycles. The first bytes up to an even address and the last
 * three bytes use the plain table T0.
 *
 * Two variants, chosen at assembly time like boltlz_m68k.S:
 *  - the 68000 (default): aligned long reads, index scaled with LSL;
 *  - -m68020 or later: any alignment, scaled index addressing, register
 *    rotates instead of a store and byte reloads.
 *
 * Arguments are on the stack (all m68k GCC ABIs), the result is in d0.
 */

/* C symbol names: ELF targets (Retro68, elf2x68k, Linux) have no prefix,
 * a.out targets (MiNT, AmigaOS) prefix an underscore. */
#ifdef __ELF__
#define SYM(x) x
#else
#define SYM(x) _##x
#endif

#if defined(__mc68020__) || defined(__mc68030__) || \
    defined(__mc68040__) || defined(__mc68060__)
#define CRC_020 1
#else
#define CRC_020 0
#endif

/* One byte through T0 (a5): d0 = state, a0 = data, d3 must have a clear
 * upper 24 bits on entry; d4 scratch. */
.macro BYTESTEP
#if CRC_020
	move.b	(%a0)+,%d3
	eor.b	%d0,%d3
	lsr.l	#8,%d0
	move.l	(%a5,%d3.w*4),%d4
	eor.l	%d4,%d0
#else
	moveq	#0,%d3
	move.b	(%a0)+,%d3
	eor.b	%d0,%d3
	lsl.w	#2,%d3
	lsr.l	#8,%d0
	move.l	(%a5,%d3.w),%d4
	eor.l	%d4,%d0
#endif
.endm

	.text
	.even
	.globl	SYM(bp_crc32_m68k)
SYM(bp_crc32_m68k):
#if CRC_020
	movem.l	%d2-%d6/%a2-%a5,-(%sp)		/* 36 bytes */
	move.l	40(%sp),%d0			/* state */
	move.l	44(%sp),%a0			/* data */
	move.l	48(%sp),%d1			/* len */
	move.l	52(%sp),%a5			/* tables: T0 */
	moveq	#0,%d3
	move.l	%d1,%d6
	lsr.l	#2,%d6				/* quads */
	beq.s	.Lc_tail
	moveq	#3,%d2
	and.l	%d2,%d1				/* tail bytes */
	/* d0 = byte-swapped state */
	ror.w	#8,%d0
	swap	%d0
	ror.w	#8,%d0
	lea	4096(%a5),%a1			/* S3 */
	lea	3072(%a5),%a2			/* S2 */
	lea	2048(%a5),%a3			/* S1 */
	lea	1024(%a5),%a4			/* S0 */
.Lc_loop:
	move.l	(%a0)+,%d2
	eor.l	%d2,%d0
	rol.l	#8,%d0
	move.b	%d0,%d3				/* byte 0 of r */
	move.l	(%a1,%d3.w*4),%d4
	rol.l	#8,%d0
	move.b	%d0,%d3
	move.l	(%a2,%d3.w*4),%d5
	eor.l	%d5,%d4
	rol.l	#8,%d0
	move.b	%d0,%d3
	move.l	(%a3,%d3.w*4),%d5
	eor.l	%d5,%d4
	rol.l	#8,%d0
	move.b	%d0,%d3
	move.l	(%a4,%d3.w*4),%d0
	eor.l	%d4,%d0
	subq.l	#1,%d6
	bne.s	.Lc_loop
	ror.w	#8,%d0
	swap	%d0
	ror.w	#8,%d0
.Lc_tail:
	subq.l	#1,%d1
	bmi.s	.Lc_done
.Lc_tl:
	BYTESTEP
	subq.l	#1,%d1
	bpl.s	.Lc_tl
.Lc_done:
	movem.l	(%sp)+,%d2-%d6/%a2-%a5
	rts
#else
	movem.l	%d2-%d6/%a2-%a5,-(%sp)		/* 36 bytes */
	subq.l	#4,%sp				/* scratch long for the state */
	move.l	44(%sp),%d0			/* state */
	move.l	48(%sp),%a0			/* data */
	move.l	52(%sp),%d1			/* len */
	move.l	56(%sp),%a5			/* tables: T0 */
	/* an even address for the long reads */
	move.l	%a0,%d2
	btst	#0,%d2
	beq.s	.Lc_even
	BYTESTEP
	subq.l	#1,%d1
.Lc_even:
	move.l	%d1,%d5
	lsr.l	#2,%d5				/* quads */
	beq.s	.Lc_tail
	moveq	#3,%d2
	and.l	%d2,%d1				/* tail bytes */
	/* d0 = byte-swapped state */
	ror.w	#8,%d0
	swap	%d0
	ror.w	#8,%d0
	lea	4096(%a5),%a1			/* S3 */
	lea	3072(%a5),%a2			/* S2 */
	lea	2048(%a5),%a3			/* S1 */
	lea	1024(%a5),%a4			/* S0 */
	subq.l	#1,%d5
	move.l	%d5,%d6
	swap	%d6				/* high word of the count */
.Lc_loop:
	move.l	(%a0)+,%d2
	eor.l	%d2,%d0
	move.l	%d0,(%sp)
	moveq	#0,%d3
	move.b	(%sp),%d3			/* byte 0 of r */
	lsl.w	#2,%d3
	move.l	(%a1,%d3.w),%d0
	moveq	#0,%d3
	move.b	1(%sp),%d3
	lsl.w	#2,%d3
	move.l	(%a2,%d3.w),%d4
	eor.l	%d4,%d0
	moveq	#0,%d3
	move.b	2(%sp),%d3
	lsl.w	#2,%d3
	move.l	(%a3,%d3.w),%d4
	eor.l	%d4,%d0
	moveq	#0,%d3
	move.b	3(%sp),%d3
	lsl.w	#2,%d3
	move.l	(%a4,%d3.w),%d4
	eor.l	%d4,%d0
	dbra	%d5,.Lc_loop
	dbra	%d6,.Lc_loop
	ror.w	#8,%d0
	swap	%d0
	ror.w	#8,%d0
.Lc_tail:
	subq.l	#1,%d1
	bmi.s	.Lc_done
.Lc_tl:
	BYTESTEP
	subq.l	#1,%d1
	bpl.s	.Lc_tl
.Lc_done:
	addq.l	#4,%sp
	movem.l	(%sp)+,%d2-%d6/%a2-%a5
	rts
#endif
