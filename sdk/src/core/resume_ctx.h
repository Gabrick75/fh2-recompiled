/*
 * Resuming a thread after an exception on Horizon.
 *
 * This is the part that is not obvious and takes a long time to find out:
 *
 * __libnx_exception_handler is not a POSIX signal handler. It runs as a normal
 * function, on the exception stack, and returning from it does not resume the
 * thread: the libnx entry code already handed control back to the kernel to jump
 * to the handler, so svcReturnFromException from here is an invalid SVC.
 *
 * The first version jumped back by hand, and it broke the game
 *
 * It reloaded the registers from the dump and did `br x16`, assuming x16 was
 * dead and leaving x18 untouched. Both assumptions are false at any instruction
 * that is not a call: GCC uses x16, x17 and x18 as normal registers inside a
 * function. In NFSMW, sub_824D2A58 read a constant from the XEX image (a
 * read-only page, unmapped), the handler emulated it, and on return x16 held the
 * resume address instead of 0x145c. The next write went to 0x80E814E4 and the
 * game closed. A manual jump always needs a register for the target, so there is
 * no fix along that path.
 *
 * How it is done now: through the kernel
 *
 * svcReturnFromException restores x0-x8, lr, sp, pc and pstate from the kernel
 * frame, and leaves the other registers as they are at that moment. So:
 *
 *   1. The dump is copied to the stack of the faulting thread, below its sp,
 *      where nothing overwrites it. The global libnx dump could be overwritten
 *      by a fault in another thread.
 *   2. NEON, x9-x28 and fp are loaded from that copy.
 *   3. An exception is raised on purpose (the udf in RexResumeTrap), with x8
 *      pointing to the copy.
 *   4. The exception entry (exception_handler_switch.cpp) recognizes the trap
 *      address, writes x0-x8, lr, sp, pc and pstate from the copy into the
 *      frame, and calls svcReturnFromException without touching anything else.
 *
 * It costs one extra exception per resume, and in exchange not a single bit is
 * lost: every register gets back the value of the corrected dump.
 */
#pragma once

#include <stddef.h>
#include <switch.h>

/*
 * extern "C" is required in C++: the symbol is defined unmangled by the assembly
 * below. Without it the link fails, and the error ("undefined reference") does not
 * point to the cause.
 */
#ifdef __cplusplus
extern "C"
#endif
__attribute__((noreturn)) void RexResumeFromException(ThreadExceptionDump* ctx);

/*
 * The assembly offsets are derived and checked here, instead of being written by
 * hand and trusted. If libnx changes the structure, this breaks at compile time
 * instead of jumping to a made-up address.
 */
_Static_assert(offsetof(ThreadExceptionDump, cpu_gprs) == 16, "gprs movidos");
_Static_assert(sizeof(CpuRegister) == 8, "CpuRegister ya no son 8 bytes");
_Static_assert(offsetof(ThreadExceptionDump, fp) == 248, "fp movido");
_Static_assert(offsetof(ThreadExceptionDump, lr) == 256, "lr movido");
_Static_assert(offsetof(ThreadExceptionDump, sp) == 264, "sp movido");
_Static_assert(offsetof(ThreadExceptionDump, pc) == 272, "pc movido");
_Static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == 288, "NEON movidos");
_Static_assert(sizeof(FpuRegister) == 16, "FpuRegister ya no son 16 bytes");
_Static_assert(offsetof(ThreadExceptionDump, pstate) == 800, "pstate movido");
_Static_assert(offsetof(ThreadExceptionDump, far) == 816, "far movido");
_Static_assert(sizeof(ThreadExceptionDump) <= 0x340, "el volcado ya no cabe en la copia");
/* The frame the kernel restores, the one the exception entry writes. */
_Static_assert(offsetof(ThreadExceptionFrameA64, lr) == 72, "lr del marco movido");
_Static_assert(offsetof(ThreadExceptionFrameA64, sp) == 80, "sp del marco movido");
_Static_assert(offsetof(ThreadExceptionFrameA64, elr_el1) == 88, "pc del marco movido");
_Static_assert(offsetof(ThreadExceptionFrameA64, pstate) == 96, "pstate del marco movido");

__asm__(
    ".text\n"
    ".balign 4\n"
    ".global RexResumeFromException\n"
    ".type RexResumeFromException, %function\n"
    "RexResumeFromException:\n"
    /*
     * 1. copy of the dump to the thread's stack: 0x340 bytes, 0x400 below its sp,
     *    16-byte aligned. It reads 8 bytes past the end of the dump, which is memory
     *    of the executable image and can be read.
     */
    "    ldr  x9,  [x0, #264]\n"
    "    sub  x9,  x9, #0x400\n"
    "    bic  x9,  x9, #0xf\n"
    "    mov  x10, x0\n"
    "    mov  x11, x9\n"
    "    add  x12, x9, #0x340\n"
    "1:  ldp  x13, x14, [x10], #16\n"
    "    stp  x13, x14, [x11], #16\n"
    "    cmp  x11, x12\n"
    "    b.lo 1b\n"
    "    mov  x8,  x9\n"
    /* 2. full NEON: the faulting access may have been floating point */
    "    add  x1,  x8, #288\n"
    "    ldp  q0,  q1,  [x1, #0]\n"
    "    ldp  q2,  q3,  [x1, #32]\n"
    "    ldp  q4,  q5,  [x1, #64]\n"
    "    ldp  q6,  q7,  [x1, #96]\n"
    "    ldp  q8,  q9,  [x1, #128]\n"
    "    ldp  q10, q11, [x1, #160]\n"
    "    ldp  q12, q13, [x1, #192]\n"
    "    ldp  q14, q15, [x1, #224]\n"
    "    ldp  q16, q17, [x1, #256]\n"
    "    ldp  q18, q19, [x1, #288]\n"
    "    ldp  q20, q21, [x1, #320]\n"
    "    ldp  q22, q23, [x1, #352]\n"
    "    ldp  q24, q25, [x1, #384]\n"
    "    ldp  q26, q27, [x1, #416]\n"
    "    ldp  q28, q29, [x1, #448]\n"
    "    ldp  q30, q31, [x1, #480]\n"
    /* x9-x28 and fp, including x16, x17 and x18 */
    "    ldp  x9,  x10, [x8, #88]\n"
    "    ldp  x11, x12, [x8, #104]\n"
    "    ldp  x13, x14, [x8, #120]\n"
    "    ldp  x15, x16, [x8, #136]\n"
    "    ldp  x17, x18, [x8, #152]\n"
    "    ldp  x19, x20, [x8, #168]\n"
    "    ldp  x21, x22, [x8, #184]\n"
    "    ldp  x23, x24, [x8, #200]\n"
    "    ldp  x25, x26, [x8, #216]\n"
    "    ldp  x27, x28, [x8, #232]\n"
    "    ldr  x29, [x8, #248]\n"
    /* 3. the trap. x0-x8, lr, sp and pc are set by the kernel. */
    ".global RexResumeTrap\n"
    "RexResumeTrap:\n"
    "    udf  #0x5245\n"
    ".size RexResumeFromException, .-RexResumeFromException\n");
