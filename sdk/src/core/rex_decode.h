/*
 * aarch64 memory access decoder for the Horizon exception handler.
 *
 * Why it is needed
 *
 * Measured on the console: on a data abort the ESR arrives with ISV = 0, meaning
 * the hardware does not fill the syndrome with the destination register or the
 * access size. Only the address (FAR) and whether it was a read or a write (WnR
 * bit of the ISS) are known. Everything else has to be recovered by decoding the
 * instruction.
 *
 * What it is used for
 *
 * On Horizon, page permissions cannot be changed on the guest window, so MMIO
 * and write watching are done by leaving the range unmapped. On a fault, the
 * handler decodes the access and emulates it by writing or reading through a
 * shadow alias of the same backing. It never makes system calls, which is the
 * Horizon restriction.
 *
 * Scope
 *
 * It covers the forms a compiler emits for recompiled code: integer accesses
 * with scaled and unscaled immediate offsets, with an index register, with
 * pre/post-increment, sign-extending loads, pairs (LDP/STP) and NEON accesses
 * from 8 to 128 bits.
 *
 * Whatever is not covered is marked as not decoded and the caller decides
 * (normally a fatal error with diagnostics). Deliberately left out:
 *   - atomics and exclusives (LDXR/STXR/LDADD/CAS...): emulating them without
 *     hardware support would be wrong as soon as there are two threads
 *   - LD1/ST1 and the other vector forms with a register list
 *   - unaligned accesses that cross the boundary of a mapped page
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <switch.h>

typedef enum {
    REX_ACC_LOAD,
    REX_ACC_STORE,
} RexAccessKind;

typedef struct {
    RexAccessKind kind;
    uint8_t  bytes;        /* 1, 2, 4, 8 o 16 */
    uint8_t  rt;           /* registro transferido */
    uint8_t  rt2;          /* second register in LDP/STP, 0xFF if none */
    uint8_t  rn;           /* base register, in case of writeback */
    bool     is_simd;      /* transfers a NEON register instead of an integer one */
    bool     sign_extend;  /* sign-extending load */
    bool     extend_to_64; /* la extension va a 64 bits, si no a 32 */
    int64_t  writeback;    /* added to the base after the access, 0 if none */
    uint64_t address;      /* direccion efectiva ya calculada */
} RexAccess;

/* --- reading registers from the dump ---------------------------------- */

static inline uint64_t RexGprRead(const ThreadExceptionDump* ctx, unsigned r) {
    if (r < 29) return ctx->cpu_gprs[r].x;
    if (r == 29) return ctx->fp.x;
    if (r == 30) return ctx->lr.x;
    return 0;  /* r31 read as the zero register */
}

/* Same, but r31 means SP: that is how address bases work. */
static inline uint64_t RexGprReadSp(const ThreadExceptionDump* ctx, unsigned r) {
    return r == 31 ? ctx->sp.x : RexGprRead(ctx, r);
}

static inline void RexGprWrite(ThreadExceptionDump* ctx, unsigned r, uint64_t v) {
    if (r < 29) ctx->cpu_gprs[r].x = v;
    else if (r == 29) ctx->fp.x = v;
    else if (r == 30) ctx->lr.x = v;
    /* r31 as destination is the zero register: discarded */
}

static inline void RexGprWriteSp(ThreadExceptionDump* ctx, unsigned r, uint64_t v) {
    if (r == 31) ctx->sp.x = v;
    else RexGprWrite(ctx, r, v);
}

/* --- auxiliares -------------------------------------------------------- */

static inline int64_t RexSignExtend(uint64_t v, unsigned bits) {
    const uint64_t m = 1ull << (bits - 1);
    return (int64_t)((v ^ m) - m);
}

/*
 * Applies the index extend option in the register forms:
 * option = 010 UXTW, 011 LSL, 110 SXTW, 111 SXTX.
 */
static inline uint64_t RexExtendIndex(uint64_t rm, unsigned option, unsigned shift) {
    uint64_t v;
    switch (option) {
        case 2: v = (uint32_t)rm; break;                       /* UXTW */
        case 6: v = (uint64_t)(int64_t)(int32_t)(uint32_t)rm; break; /* SXTW */
        case 3:                                                 /* LSL  */
        case 7:                                                 /* SXTX */
        default: v = rm; break;
    }
    return v << shift;
}

/*
 * --- the decoder -------------------------------------------------------
 *
 * Returns true if it recognized the access and filled in out.
 */
static inline bool RexDecodeAccess(const ThreadExceptionDump* ctx, uint32_t insn,
                                   RexAccess* out) {
    out->rt2 = 0xFF;
    out->writeback = 0;
    out->sign_extend = false;
    out->extend_to_64 = false;
    out->is_simd = false;

    const unsigned rt = insn & 0x1F;
    const unsigned rn = (insn >> 5) & 0x1F;
    out->rt = (uint8_t)rt;
    out->rn = (uint8_t)rn;

    const uint64_t base = RexGprReadSp(ctx, rn);

    /* ---- LDP / STP -----------------------------------------------------
     * opc:2 101 V:1 0 idx:2 L:1 imm7 Rt2 Rn Rt
     * idx: 01 post-incremento, 10 desplazamiento con signo, 11 pre-incremento
     */
    if ((insn & 0x3A000000u) == 0x28000000u) {
        const unsigned opc = (insn >> 30) & 0x3;
        const unsigned v   = (insn >> 26) & 0x1;
        const unsigned idx = (insn >> 23) & 0x3;
        const unsigned L   = (insn >> 22) & 0x1;
        const unsigned rt2 = (insn >> 10) & 0x1F;
        const int64_t imm7 = RexSignExtend((insn >> 15) & 0x7F, 7);

        if (idx == 0) return false;  /* forma sin asignar */

        unsigned bytes;
        if (v) {
            /* NEON: opc 00 = 4 bytes, 01 = 8, 10 = 16 */
            if (opc > 2) return false;
            bytes = 4u << opc;
        } else {
            /* enteros: opc 00 = 4 bytes, 10 = 8. opc 01 es LDPSW */
            if (opc == 0) bytes = 4;
            else if (opc == 2) bytes = 8;
            else if (opc == 1 && L) { bytes = 4; out->sign_extend = true; out->extend_to_64 = true; }
            else return false;
        }

        const int64_t offset = imm7 * (int64_t)bytes;
        out->kind    = L ? REX_ACC_LOAD : REX_ACC_STORE;
        out->bytes   = (uint8_t)bytes;
        out->rt2     = (uint8_t)rt2;
        out->is_simd = v != 0;
        out->address = base + (idx == 1 ? 0 : offset);  /* post-inc: sin offset */
        out->writeback = (idx == 1 || idx == 3) ? offset : 0;
        return true;
    }

    /*
     * ---- LDR / STR family ---------------------------------------------
     * size:2 111 V:1 xx opc:2 ...
     *
     * Bits [25:24] separate two groups:
     *   00  index register, unscaled (LDUR/STUR), pre- and post-increment
     *   01  12-bit immediate offset, scaled by the size
     *
     * The mask leaves bit 24 free on purpose to catch both. Fixing it to 0 (the
     * first attempt) rejects exactly the most common form the compiler emits, and
     * the symptom is puzzling: the rare forms work and the normal ones fail.
     *
     * Bit 28 tells this family apart from the pairs (1 here, 0 in LDP/STP), so
     * there is no collision with the block above.
     */
    if ((insn & 0x3A000000u) == 0x38000000u) {
        const unsigned size = (insn >> 30) & 0x3;
        const unsigned v    = (insn >> 26) & 0x1;
        const unsigned opc  = (insn >> 22) & 0x3;

        /* size: for NEON the high bit of opc extends it to 128 bits */
        unsigned bytes;
        if (v) {
            const unsigned sz = size | ((opc & 2) << 1);  /* 0..4 */
            if (sz > 4) return false;
            bytes = 1u << sz;
        } else {
            bytes = 1u << size;
        }

        bool is_load;
        if (v) {
            is_load = (opc & 1) != 0;
        } else {
            switch (opc) {
                case 0: is_load = false; break;               /* STR  */
                case 1: is_load = true;  break;               /* LDR  */
                case 2:                                        /* LDRS -> 64 */
                case 3:                                        /* LDRS -> 32 */
                    if (size == 3) return false;              /* PRFM or others */
                    is_load = true;
                    out->sign_extend = true;
                    out->extend_to_64 = (opc == 2);
                    break;
                default: return false;
            }
        }

        out->kind    = is_load ? REX_ACC_LOAD : REX_ACC_STORE;
        out->bytes   = (uint8_t)bytes;
        out->is_simd = v != 0;

        if ((insn & 0x01000000u) != 0) {
            /* unsigned immediate offset, scaled by the size */
            const uint64_t imm12 = (insn >> 10) & 0xFFF;
            unsigned shift = size;
            if (v && (opc & 2)) shift = 4;   /* NEON de 128 bits */
            out->address = base + (imm12 << shift);
            return true;
        }

        const unsigned modo = (insn >> 10) & 0x3;
        if (modo == 2) {
            /* with index register */
            const unsigned rm     = (insn >> 16) & 0x1F;
            const unsigned option = (insn >> 13) & 0x7;
            const unsigned S      = (insn >> 12) & 0x1;
            unsigned shift = 0;
            if (S) {
                shift = size;
                if (v && (opc & 2)) shift = 4;
            }
            out->address = base + RexExtendIndex(RexGprRead(ctx, rm), option, shift);
            return true;
        }

        /* signed 9-bit immediate, unscaled */
        const int64_t imm9 = RexSignExtend((insn >> 12) & 0x1FF, 9);
        if (modo == 0) {           /* LDUR / STUR */
            out->address = base + imm9;
            return true;
        }
        if (modo == 1) {           /* post-incremento */
            out->address = base;
            out->writeback = imm9;
            return true;
        }
        if (modo == 3) {           /* pre-incremento */
            out->address = base + imm9;
            out->writeback = imm9;
            return true;
        }
        return false;
    }

    return false;  /* no reconocido: atomicos, vectoriales con lista, etc. */
}

/*
 * --- emulation ---------------------------------------------------------
 *
 * shadow is the address in the shadow alias that corresponds to acc->address.
 * The caller computes it: sombra_base + (acc->address - ventana_base).
 */
static inline void RexEmulateAccess(ThreadExceptionDump* ctx, const RexAccess* acc,
                                    void* shadow) {
    uint8_t* p = (uint8_t*)shadow;

    if (acc->kind == REX_ACC_LOAD) {
        if (acc->is_simd) {
            uint8_t* dst = (uint8_t*)&ctx->fpu_gprs[acc->rt];
            memset(dst, 0, 16);
            memcpy(dst, p, acc->bytes);
            if (acc->rt2 != 0xFF) {
                uint8_t* d2 = (uint8_t*)&ctx->fpu_gprs[acc->rt2];
                memset(d2, 0, 16);
                memcpy(d2, p + acc->bytes, acc->bytes);
            }
        } else {
            uint64_t v = 0;
            memcpy(&v, p, acc->bytes);
            if (acc->sign_extend) {
                v = (uint64_t)RexSignExtend(v, acc->bytes * 8);
                if (!acc->extend_to_64) v = (uint32_t)v;
            } else if (acc->bytes < 8) {
                /* a 32-bit load zeroes the upper half */
                v &= (acc->bytes == 4) ? 0xFFFFFFFFull : ((1ull << (acc->bytes * 8)) - 1);
            }
            RexGprWrite(ctx, acc->rt, v);

            if (acc->rt2 != 0xFF) {
                uint64_t v2 = 0;
                memcpy(&v2, p + acc->bytes, acc->bytes);
                if (acc->sign_extend) v2 = (uint64_t)RexSignExtend(v2, acc->bytes * 8);
                else if (acc->bytes == 4) v2 &= 0xFFFFFFFFull;
                RexGprWrite(ctx, acc->rt2, v2);
            }
        }
    } else {
        if (acc->is_simd) {
            memcpy(p, &ctx->fpu_gprs[acc->rt], acc->bytes);
            if (acc->rt2 != 0xFF)
                memcpy(p + acc->bytes, &ctx->fpu_gprs[acc->rt2], acc->bytes);
        } else {
            const uint64_t v = RexGprRead(ctx, acc->rt);
            memcpy(p, &v, acc->bytes);
            if (acc->rt2 != 0xFF) {
                const uint64_t v2 = RexGprRead(ctx, acc->rt2);
                memcpy(p + acc->bytes, &v2, acc->bytes);
            }
        }
    }

    if (acc->writeback)
        RexGprWriteSp(ctx, acc->rn, RexGprReadSp(ctx, acc->rn) + acc->writeback);
}
