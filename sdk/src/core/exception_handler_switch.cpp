/**
 * @file        rex/core/exception_handler_switch.cpp
 * @brief       ExceptionHandler for Horizon, on top of a single __libnx_exception_handler
 *
 * How a fault arrives on Horizon, which is not how it does on POSIX
 *
 * All measured on the console (see docs/platform-notes.md), not assumed:
 *
 *  - There are no signals. A fault reaches __libnx_exception_handler, which libnx calls
 *    as a normal function on a stack of its own.
 *  - Returning from it kills the process. To continue, RexResumeFromException
 *    (resume_ctx.h) raises a trap exception and the entry below returns the corrected
 *    context with svcReturnFromException.
 *  - The ESR carries the WnR bit (read or write) but arrives with ISV = 0: it gives
 *    neither the register nor the size. Emulating an access requires decoding.
 *  - libnx accepts a single handler. What on POSIX are separate signal handlers (this
 *    one and the SEH one) must all go through here.
 *
 * The decision order, and why
 *
 *  0. Uncommitted guest physical memory -> 4 MB is committed and the access retried.
 *     Physical memory is no longer committed in full at startup: on Horizon those
 *     512 MB are counted once for each of the 360's five views and the kernel ends
 *     up refusing allocations (see xmemory.cpp).
 *
 *  1. Read from a watched guest page -> emulated through the shadow and never reaches
 *     the SDK. It has to be like this: on Switch a read-only page is unmapped, and if
 *     the fault reached the MMIO handler, it would see kReadOnly, conclude that another
 *     thread already removed the watch and retry the instruction... which would fault
 *     again, forever.
 *
 *  2. The handlers installed by the SDK (MMIO, write watching), in installation order,
 *     as on POSIX.
 *
 *     If one reports "handled" and leaves the PC where it was (that is, asks for a
 *     retry), whether the page ended up mapped is checked right then. If it did not,
 *     the retry would loop forever, so it is emulated through the shadow. This covers
 *     the case where remapping from the handler does not work on Horizon, without
 *     relying on heuristics.
 *
 *  3. SEH, if in use. See seh_switch.cpp.
 *
 *  4. Fatal: registers, address and stack to <NRO folder>/logs/rex/rex_crash.log, and
 *     error 2345-0102 on screen.
 *
 * Faults on several threads at once, and why they broke the game
 *
 * libnx has one exception stack and one dump, both global. With a single thread that
 * works; here it does not. The profiler counts hundreds or thousands of faults per
 * second spread over twenty-odd threads, so two coincide every so often, and then:
 *
 *  - the second thread writes its dump over the first one's, and the first resumes
 *    with the second one's registers (silent guest corruption, impossible to track);
 *  - both run on the same stack and overwrite each other's frames.
 *
 * Measured on the console while loading a race: two threads inside the handler with
 * their sp 0xD0 apart, one waiting for the guest memory lock and the other with a
 * mutex pointer turned into 1. The latter killed the process (report in
 * rex_crash.log, 2345-0102).
 *
 * Fix: REX_EXC_RANURAS stack + dump sets. The entry takes a free one with
 * ldaxr/stlxr, points the dump and sp at it, and stores the slot number in pad[0] of
 * the dump. On resume, the trap branch reads it from the copy and releases the slot:
 * at that point the thread no longer runs on that stack, so another one can take it
 * without overwriting anything. If none is free it waits; waiting is better than
 * corrupting.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

#include <rex/exception_handler.h>

#include <switch.h>

#include "guest_memory_switch.h"
#include "resume_ctx.h"
#include "rex_decode.h"

/* Provided by seh_switch.cpp. Returns true if it redirected execution. */
extern "C" bool RexSwitchSehHandleFault(ThreadExceptionDump* ctx);

/* Performance profiler counters (switch_perf.cpp). */
extern "C" void RexSwitchPerfCount(unsigned id);
// rexglue log folder, ending in '/' (switch_crash_hooks.c).
extern "C" const char* RexSwitchLogDir(void);

/*
 * The exception slots: one stack and one dump per thread faulting at the same time. See the header.
 * The sizes are powers of two on purpose, so the assembly reaches slot N with a shift rather than a
 * multiplication.
 *
 * Eight is plenty: measurements never showed more than two threads inside the handler at once, and
 * they are 512 KB of .bss, which take no space in the NRO.
 */
#define REX_EXC_RANURAS 8
#define REX_EXC_PILA 0x10000   /* 64 KB per slot -> shift of 16 */
#define REX_EXC_VOLCADO 0x400  /* the dump takes 0x340 -> shift of 10 */

extern "C" {
alignas(16) uint8_t g_rex_exc_pilas[REX_EXC_RANURAS][REX_EXC_PILA];
alignas(16) uint8_t g_rex_exc_volcados[REX_EXC_RANURAS][REX_EXC_VOLCADO];
alignas(16) uint32_t g_rex_exc_ocupadas[REX_EXC_RANURAS];
}

static_assert(sizeof(ThreadExceptionDump) <= REX_EXC_VOLCADO,
              "el volcado ya no cabe en la ranura");
static_assert(REX_EXC_RANURAS == 8, "el ensamblador compara contra 8 a mano");

/*
 * The exception entry, replacing libnx's
 *
 * libnx defines __libnx_exception_entry as a weak symbol, and crt0 jumps to it with x0 = exception
 * type and x1 = kernel frame. This is libnx 4.12's one instruction by instruction (read from the
 * linked binary), with one extra branch at the start: if the frame's pc is RexResumeTrap, the
 * exception is the resume from resume_ctx.h, not a fault. Then x0-x8, lr, sp, pc and pstate are
 * copied into the frame from the dump x8 points to, and it returns with svcReturnFromException
 * without touching x9-x29 or NEON, which already hold their values.
 *
 * The rest is libnx's: bail out if a debugger is attached (unless __nx_exception_ignoredebug), copy
 * the frame and the live registers to __nx_exceptiondump, and return from the kernel to
 * RexExceptionReturnEntry on the exception stack with x0 pointing to the dump.
 */
__asm__(".text\n"
        ".balign 4\n"
        ".global __libnx_exception_entry\n"
        ".type __libnx_exception_entry, %function\n"
        "__libnx_exception_entry:\n"
        "    cbz  x1, .Lrex_exc_abort\n"
        "    ldr  x2, [x1, #88]\n"
        "    adrp x3, RexResumeTrap\n"
        "    add  x3, x3, :lo12:RexResumeTrap\n"
        "    cmp  x2, x3\n"
        "    b.eq .Lrex_exc_resume\n"
        /* debugger? svcGetInfo(InfoType_DebuggerAttached) preserving x9-x21 */
        "    stp  x9,  x10, [sp, #-16]!\n"
        "    stp  x11, x12, [sp, #-16]!\n"
        "    stp  x13, x14, [sp, #-16]!\n"
        "    stp  x15, x16, [sp, #-16]!\n"
        "    stp  x17, x18, [sp, #-16]!\n"
        "    stp  x19, x20, [sp, #-16]!\n"
        "    str  x21, [sp, #-16]!\n"
        "    stp  x0,  x1,  [sp, #-16]!\n"
        "    sub  sp, sp, #16\n"
        "    mov  x0, sp\n"
        "    mov  x1, #8\n"
        "    mov  w2, wzr\n"
        "    mov  x3, #0\n"
        "    bl   svcGetInfo\n"
        "    mov  w6, w0\n"
        "    ldr  x7, [sp], #16\n"
        "    ldp  x0,  x1,  [sp], #16\n"
        "    ldr  x21, [sp], #16\n"
        "    ldp  x19, x20, [sp], #16\n"
        "    ldp  x17, x18, [sp], #16\n"
        "    ldp  x15, x16, [sp], #16\n"
        "    ldp  x13, x14, [sp], #16\n"
        "    ldp  x11, x12, [sp], #16\n"
        "    ldp  x9,  x10, [sp], #16\n"
        "    cbnz w6, .Lrex_exc_abort\n"
        "    adrp x6, __nx_exception_ignoredebug\n"
        "    ldr  w5, [x6, :lo12:__nx_exception_ignoredebug]\n"
        "    cbnz w5, .Lrex_exc_dump\n"
        "    cbnz x7, .Lrex_exc_abort\n"
        /*
         * Take a free slot. Only x3-x7 are touched: x0 holds the exception type and x1
         * the kernel frame, and both are needed below.
         */
        ".Lrex_exc_dump:\n"
        "    adrp x6, g_rex_exc_ocupadas\n"
        "    add  x6, x6, :lo12:g_rex_exc_ocupadas\n"
        "    mov  w7, wzr\n"
        ".Lrex_exc_pide:\n"
        "    add  x5, x6, x7, lsl #2\n"
        "    ldaxr w4, [x5]\n"
        "    cbnz w4, .Lrex_exc_siguiente\n"
        "    mov  w3, #1\n"
        "    stlxr w4, w3, [x5]\n"
        "    cbz  w4, .Lrex_exc_cogida\n"
        "    b    .Lrex_exc_pide\n"
        ".Lrex_exc_siguiente:\n"
        "    clrex\n"
        "    add  w7, w7, #1\n"
        "    cmp  w7, #8\n"
        "    b.lo .Lrex_exc_pide\n"
        /*
         * All taken: go back to the first one and wait. A thread stalled here is far
         * less harmful than two writing to the same dump.
         */
        "    mov  w7, wzr\n"
        "    b    .Lrex_exc_pide\n"
        ".Lrex_exc_cogida:\n"
        /*
         * This slot's dump, with the slot number in pad[0], which the resume branch
         * reads from the copy to release it.
         */
        "    adrp x2, g_rex_exc_volcados\n"
        "    add  x2, x2, :lo12:g_rex_exc_volcados\n"
        "    add  x2, x2, x7, lsl #10\n"
        "    mov  x5, x2\n"
        "    str  w0,  [x2], #4\n"
        "    str  w7,  [x2], #4\n"
        "    str  wzr, [x2], #4\n"
        "    str  wzr, [x2], #4\n"
        "    ldp  x3, x4, [x1]\n"
        "    str  x5, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldp  x3, x4, [x1], #16\n"
        "    stp  x3, x4, [x2], #16\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    str  x9, [x2], #8\n"
        "    stp  x10, x11, [x2], #16\n"
        "    stp  x12, x13, [x2], #16\n"
        "    stp  x14, x15, [x2], #16\n"
        "    stp  x16, x17, [x2], #16\n"
        "    stp  x18, x19, [x2], #16\n"
        "    stp  x20, x21, [x2], #16\n"
        "    stp  x22, x23, [x2], #16\n"
        "    stp  x24, x25, [x2], #16\n"
        "    stp  x26, x27, [x2], #16\n"
        "    str  x28, [x2], #8\n"
        "    str  x29, [x2], #8\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        /* Top of this slot's stack: base + (ranura + 1) * 64 KB. */
        "    adrp x4, g_rex_exc_pilas\n"
        "    add  x4, x4, :lo12:g_rex_exc_pilas\n"
        "    add  x7, x7, #1\n"
        "    add  x4, x4, x7, lsl #16\n"
        "    ldr  x3, [x1]\n"
        "    str  x4, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    adrp x4, RexExceptionReturnEntry\n"
        "    add  x4, x4, :lo12:RexExceptionReturnEntry\n"
        "    ldr  x3, [x1]\n"
        "    str  x4, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    str  xzr, [x2], #8\n"
        "    stp  q0,  q1,  [x2], #32\n"
        "    stp  q2,  q3,  [x2], #32\n"
        "    stp  q4,  q5,  [x2], #32\n"
        "    stp  q6,  q7,  [x2], #32\n"
        "    stp  q8,  q9,  [x2], #32\n"
        "    stp  q10, q11, [x2], #32\n"
        "    stp  q12, q13, [x2], #32\n"
        "    stp  q14, q15, [x2], #32\n"
        "    stp  q16, q17, [x2], #32\n"
        "    stp  q18, q19, [x2], #32\n"
        "    stp  q20, q21, [x2], #32\n"
        "    stp  q22, q23, [x2], #32\n"
        "    stp  q24, q25, [x2], #32\n"
        "    stp  q26, q27, [x2], #32\n"
        "    stp  q28, q29, [x2], #32\n"
        "    stp  q30, q31, [x2], #32\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  w3, [x1], #4\n"
        "    str  w3, [x2], #4\n"
        "    ldr  x3, [x1], #8\n"
        "    str  x3, [x2], #8\n"
        "    mov  w0, wzr\n"
        "    b    .Lrex_exc_end\n"
        /* resume: frame <- copied dump (x8 of the frame) */
        ".Lrex_exc_resume:\n"
        "    ldr  x2, [x1, #64]\n"
        /*
         * Release the slot. The thread no longer runs on the exception stack here
         * (it is inside the kernel, about to return to the faulting code), so another
         * thread can take it without overwriting anything. The mask is in case the
         * copied dump were damaged.
         */
        "    ldr  w3, [x2, #4]\n"
        "    and  w3, w3, #7\n"
        "    adrp x4, g_rex_exc_ocupadas\n"
        "    add  x4, x4, :lo12:g_rex_exc_ocupadas\n"
        "    add  x4, x4, x3, lsl #2\n"
        "    stlr wzr, [x4]\n"
        "    ldp  x3, x4, [x2, #16]\n"
        "    stp  x3, x4, [x1, #0]\n"
        "    ldp  x3, x4, [x2, #32]\n"
        "    stp  x3, x4, [x1, #16]\n"
        "    ldp  x3, x4, [x2, #48]\n"
        "    stp  x3, x4, [x1, #32]\n"
        "    ldp  x3, x4, [x2, #64]\n"
        "    stp  x3, x4, [x1, #48]\n"
        "    ldr  x3, [x2, #80]\n"
        "    str  x3, [x1, #64]\n"
        "    ldp  x3, x4, [x2, #256]\n"
        "    stp  x3, x4, [x1, #72]\n"
        "    ldr  x3, [x2, #272]\n"
        "    str  x3, [x1, #88]\n"
        "    ldr  w3, [x2, #800]\n"
        "    str  w3, [x1, #96]\n"
        "    mov  w0, wzr\n"
        "    b    .Lrex_exc_end\n"
        ".Lrex_exc_abort:\n"
        "    mov  w0, #0xf801\n"
        ".Lrex_exc_end:\n"
        "    bl   svcReturnFromException\n"
        "    b    .\n"
        ".size __libnx_exception_entry, .-__libnx_exception_entry\n"
        "\n"
        ".balign 4\n"
        ".type RexExceptionReturnEntry, %function\n"
        "RexExceptionReturnEntry:\n"
        "    bl   __libnx_exception_handler\n"
        "    mov  w0, wzr\n"
        "    mov  x1, #0\n"
        "    mov  x2, #0\n"
        "    bl   svcBreak\n"
        "    b    .\n"
        ".size RexExceptionReturnEntry, .-RexExceptionReturnEntry\n");

/*
 * NFSMW's first boot went back to the HOME menu right after creating the GPU threads: no error on
 * screen, no Atmosphere report and the log cut off abruptly. The previous stop (a brk inside the
 * handler, with the data in x24..x27 for the report) left nothing to read. So the fault is written
 * right here, to <NRO folder>/logs/rex/rex_crash.log.
 *
 * It is written with the fs service directly, not with stdio: the faulting thread may have been left
 * halfway inside newlib, holding its locks.
 *
 * abort() and exit() use it too, see switch_crash_hooks.c. That is why ctx can be null: then there
 * are no registers, only the stack from 'stack'.
 */
extern "C" void _start(void);

namespace {

struct CrashBuf {
  char data[16384];
  size_t len = 0;
};

__attribute__((format(printf, 2, 3))) void Append(CrashBuf& b, const char* fmt, ...) {
  if (b.len + 1 >= sizeof(b.data)) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  const int n = std::vsnprintf(b.data + b.len, sizeof(b.data) - b.len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    b.len = std::min(sizeof(b.data) - 1, b.len + static_cast<size_t>(n));
  }
}

/* Region of 'addr' if it is readable and spans 'bytes'. */
bool QueryReadable(uint64_t addr, size_t bytes, MemoryInfo* out) {
  u32 page_info = 0;
  if (R_FAILED(svcQueryMemory(out, &page_info, addr))) {
    return false;
  }
  return out->type != MemType_Unmapped && (out->perm & Perm_R) &&
         addr + bytes <= out->addr + out->size;
}

/*
 * Without frame pointers (GCC omits them at -O2 on aarch64) there is no chain to follow. The stack
 * is walked and the values that fall in the executable's code right after a BL or BLR are recorded:
 * they are almost certainly return addresses. Some may be leftovers from an earlier call; the order
 * matters more than any single line.
 */
void ScanStack(CrashBuf& b, uint64_t stack, uint64_t base, uint64_t text_lo, uint64_t text_hi) {
  stack &= ~uint64_t(7);
  MemoryInfo mi;
  if (!QueryReadable(stack, 8, &mi)) {
    Append(b, "pila ilegible en 0x%016" PRIx64 "\n", stack);
    return;
  }
  const uint64_t end = std::min(mi.addr + mi.size, stack + 0x10000);
  Append(b, "posibles retornos en la pila (0x%016" PRIx64 "):\n", stack);
  int hits = 0;
  for (uint64_t p = stack; p + 8 <= end && hits < 48; p += 8) {
    const uint64_t v = *reinterpret_cast<const uint64_t*>(p);
    if (v < text_lo + 4 || v >= text_hi || (v & 3)) {
      continue;
    }
    const uint32_t prev = *reinterpret_cast<const uint32_t*>(v - 4);
    const bool bl = (prev & 0xFC000000u) == 0x94000000u;
    const bool blr = (prev & 0xFFFFFC1Fu) == 0xD63F0000u;
    if (!bl && !blr) {
      continue;
    }
    Append(b, "  sp+0x%05" PRIx64 "  imagen+0x%" PRIx64 "\n", p - stack, v - base);
    ++hits;
  }
}

void WriteCrashFile(const char* data, size_t len) {
  FsFileSystem* fs = nullptr;
  char path[FS_MAX_PATH];
  // In <NRO folder>/logs/rex/ (switch_crash_hooks.c computes it at startup, without allocating here).
  // If that path cannot be translated, the default one.
  char ruta[FS_MAX_PATH];
  std::snprintf(ruta, sizeof(ruta), "%srex_crash.log", RexSwitchLogDir());
  if ((fsdevTranslatePath(ruta, &fs, path) < 0 || !fs) &&
      (fsdevTranslatePath("sdmc:/switch/rex_crash.log", &fs, path) < 0 || !fs)) {
    return;
  }
  fsFsCreateFile(fs, path, 0, 0);  // fails if it already exists, which is fine
  FsFile f;
  if (R_FAILED(fsFsOpenFile(fs, path, FsOpenMode_Write | FsOpenMode_Append, &f))) {
    return;
  }
  s64 size = 0;
  fsFileGetSize(&f, &size);
  fsFileWrite(&f, size, data, len, FsWriteOption_Flush);
  fsFileClose(&f);
}

}  // namespace

extern "C" void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx,
                                  uint64_t stack, uint64_t pc) {
  // A second fault while the first is being written (on this thread or another)
  // does not write: the first one is what explains what happened.
  static std::atomic_flag busy = ATOMIC_FLAG_INIT;
  if (busy.test_and_set()) {
    return;
  }
  static CrashBuf b;
  b.len = 0;

  const uint64_t base = reinterpret_cast<uint64_t>(&_start);
  uint64_t text_lo = base, text_hi = base;
  MemoryInfo mi;
  if (QueryReadable(base, 4, &mi)) {
    text_lo = mi.addr;
    text_hi = mi.addr + mi.size;
  }
  u64 tid = 0;
  svcGetThreadId(&tid, CUR_THREAD_HANDLE);

  Append(b, "==== %s ====\n", reason ? reason : "?");
  Append(b, "hilo %" PRIu64 ", imagen 0x%016" PRIx64 " (codigo hasta 0x%016" PRIx64 ")\n", tid,
         base, text_hi);
  Append(b, "pc 0x%016" PRIx64 " = imagen+0x%" PRIx64 "\n", pc, pc - base);

  if (ctx) {
    const uint32_t esr = ctx->esr;
    Append(b, "lr 0x%016" PRIx64 " = imagen+0x%" PRIx64 "\n", ctx->lr.x, ctx->lr.x - base);
    Append(b, "esr 0x%08x (EC 0x%02x)  far 0x%016" PRIx64 "  error_desc 0x%x  pstate 0x%08x\n",
           esr, esr >> 26, ctx->far.x, ctx->error_desc, ctx->pstate);
    if (QueryReadable(pc, 4, &mi) && (mi.perm & Perm_X)) {
      Append(b, "instruccion 0x%08x\n", *reinterpret_cast<const uint32_t*>(pc));
    }
    MemoryInfo fm;
    u32 page_info = 0;
    if (R_SUCCEEDED(svcQueryMemory(&fm, &page_info, ctx->far.x))) {
      Append(b,
             "far en region 0x%016" PRIx64 "+0x%" PRIx64 " tipo 0x%x permisos 0x%x\n",
             fm.addr, fm.size, fm.type, fm.perm);
    }
    const uint64_t gm = reinterpret_cast<uint64_t>(RexGmBase());
    if (gm && ctx->far.x >= gm && ctx->far.x < gm + RexGmSize()) {
      Append(b, "far dentro de la memoria del invitado: 0x%08" PRIx64 "\n", ctx->far.x - gm);
    }
    for (int i = 0; i < 29; ++i) {
      Append(b, "x%-2d 0x%016" PRIx64 "%s", i, ctx->cpu_gprs[i].x, (i % 3 == 2) ? "\n" : "  ");
    }
    Append(b, "fp  0x%016" PRIx64 "  sp  0x%016" PRIx64 "\n", ctx->fp.x, ctx->sp.x);
  }

  // Frame chain: the ELF keeps x29 as the frame pointer (the Atmosphere report of
  // the first abort walked it completely). More reliable than the scan below,
  // which remains as a fallback for when the chain breaks.
  Append(b, "cadena de marcos:\n");
  uint64_t frame = ctx ? ctx->fp.x : stack;
  for (int i = 0; i < 40 && (frame & 7) == 0 && QueryReadable(frame, 16, &mi); ++i) {
    const uint64_t next = reinterpret_cast<const uint64_t*>(frame)[0];
    const uint64_t ret = reinterpret_cast<const uint64_t*>(frame)[1];
    if (ret >= text_lo && ret < text_hi) {
      Append(b, "  #%02d imagen+0x%" PRIx64 "\n", i, ret - base);
    } else {
      Append(b, "  #%02d 0x%016" PRIx64 "\n", i, ret);
    }
    if (next <= frame) {
      break;
    }
    frame = next;
  }

  ScanStack(b, ctx ? ctx->sp.x : stack, base, text_lo, text_hi);
  Append(b, "\n");
  WriteCrashFile(b.data, b.len);
  busy.clear();
}

namespace rex::arch {

static_assert(sizeof(vec128_t) == 16, "vec128_t ya no mide 16 bytes");
static_assert(sizeof(FpuRegister) == 16, "FpuRegister ya no mide 16 bytes");

namespace {

// As on POSIX: few, because they are walked on every fault.
constexpr size_t kMaxHandlerCount = 8;
std::pair<ExceptionHandler::Handler, void*> handlers_[kMaxHandlerCount];

constexpr uint32_t kEsrEcShift = 26;
constexpr uint32_t kEcDataAbortMask = 0b111110;
constexpr uint32_t kEcDataAbort = 0b100100;  // 0x24 (lower EL) and 0x25 (same EL)
constexpr uint32_t kEcUnknown = 0x00;        // undefined instructions arrive here

bool InGuestWindow(uint64_t addr) {
  const uint8_t* base = RexGmBase();
  if (!base) {
    return false;
  }
  const uint64_t b = reinterpret_cast<uint64_t>(base);
  return addr >= b && addr < b + RexGmSize();
}

/*
 * Emulates the access of the instruction at PC by reading or writing through the shadow, and
 * advances the PC. Returns false if it could not: unrecognized instruction, no backing, or an
 * access that crosses into another backing chunk (the shadow is not contiguous across chunks, so it
 * cannot be emulated in one go).
 */
bool EmulateViaShadow(ThreadExceptionDump* ctx) {
  const uint32_t insn = *reinterpret_cast<const uint32_t*>(ctx->pc.x);
  RexAccess acc;
  if (!RexDecodeAccess(ctx, insn, &acc)) {
    return false;
  }
  const size_t total = static_cast<size_t>(acc.bytes) * (acc.rt2 != 0xFF ? 2 : 1);
  void* ini = RexGmShadowFor(acc.address);
  void* fin = RexGmShadowFor(acc.address + total - 1);
  if (!ini || !fin || static_cast<uint8_t*>(fin) != static_cast<uint8_t*>(ini) + total - 1) {
    return false;
  }
  RexEmulateAccess(ctx, &acc, ini);
  ctx->pc.x += 4;
  return true;
}

void FillContext(HostThreadContext& tc, const ThreadExceptionDump* ctx) {
  for (int i = 0; i < 29; ++i) {
    tc.x[i] = ctx->cpu_gprs[i].x;
  }
  tc.x[29] = ctx->fp.x;
  tc.x[30] = ctx->lr.x;
  tc.sp = ctx->sp.x;
  tc.pc = ctx->pc.x;
  tc.pstate = ctx->pstate;

  // The Horizon dump has no FPSR or FPCR. They are thread registers and the
  // handler runs on the same thread, so they are read live.
  uint64_t fpsr = 0, fpcr = 0;
  __asm__ volatile("mrs %0, fpsr\n mrs %1, fpcr" : "=r"(fpsr), "=r"(fpcr));
  tc.fpsr = static_cast<uint32_t>(fpsr);
  tc.fpcr = static_cast<uint32_t>(fpcr);

  for (int i = 0; i < 32; ++i) {
    std::memcpy(&tc.v[i], &ctx->fpu_gprs[i], sizeof(vec128_t));
  }
}

/*
 * Copies to the dump what the SDK handler changed. Only the registers it marked as modified, as on
 * POSIX.
 */
void WriteBack(const Exception& ex, const HostThreadContext& tc, ThreadExceptionDump* ctx) {
  uint32_t xm = ex.modified_x_registers();
  while (xm) {
    const unsigned i = static_cast<unsigned>(__builtin_ctz(xm));
    xm &= xm - 1;
    if (i < 29) {
      ctx->cpu_gprs[i].x = tc.x[i];
    } else if (i == 29) {
      ctx->fp.x = tc.x[29];
    } else {
      ctx->lr.x = tc.x[30];
    }
  }
  uint32_t vm = ex.modified_v_registers();
  while (vm) {
    const unsigned i = static_cast<unsigned>(__builtin_ctz(vm));
    vm &= vm - 1;
    std::memcpy(&ctx->fpu_gprs[i], &tc.v[i], sizeof(FpuRegister));
  }
  ctx->sp.x = tc.sp;
  ctx->pc.x = tc.pc;
  ctx->pstate = static_cast<u32>(tc.pstate);
}

[[noreturn]] void HandleSwitchException(ThreadExceptionDump* ctx) {
  const uint64_t pc = ctx->pc.x;
  const uint64_t far = ctx->far.x;
  const uint32_t esr = ctx->esr;
  const uint32_t ec = esr >> kEsrEcShift;
  const bool data_abort = (ec & kEcDataAbortMask) == kEcDataAbort;
  const bool is_write = data_abort && ((esr >> 6) & 1);
  const bool in_window = data_abort && InGuestWindow(far);

  /*
   * The hot path, from five locks down to two
   *
   * Measured in a race: up to 113,000 read faults per second, and the thread that suffered most went
   * to 76% of a core. What took the time was not resolving the fault: it was asking.
   *
   * A read fault on a watched page used to go through here like this:
   *   1. RexGmFaultIn          -> lock, checks permissions, says no
   *   2. RexGmWindowToOffset   -> lock, looks up the view again
   *   3. RexGmQueryProtect     -> lock, looks up view and chunk again
   *   4. RexGmQueryProtect     -> lock, the same for the third time
   *   5. RexGmShadowFor        -> lock, looks up view and chunk again
   *
   * Five locks and a dozen tree lookups to always answer the same thing. And with three threads
   * faulting at once on the same lock, they queued up: total CPU stayed at 250% of 300% without any of
   * them reaching 100%.
   *
   * Now it asks once, at the top, and the common case (reading from a watched page) is resolved right
   * there with the shadow. Two remain: the query and the shadow.
   */
  if (data_abort && in_window) {
    size_t largo = 0x1000;
    RexGmAccess acceso = REX_GM_NONE;
    const bool hay_permiso =
        RexGmQueryProtect(reinterpret_cast<uint8_t*>(far), &largo, &acceso);

    /*
     * (1) Read from a watched page. By far the common case, so it goes first and asks nothing
     * again. It is still emulated through the shadow: see below for why treating it as a write
     * does not work.
     */
    if (!is_write && hay_permiso && acceso == REX_GM_READ && EmulateViaShadow(ctx)) {
      RexSwitchPerfCount(1);
      RexResumeFromException(ctx);
    }

    /* (2) Committed, but not yet mapped in this view. */
    if (RexGmFaultIn(far)) {
      RexSwitchPerfCount(18);
      RexResumeFromException(ctx);
    }

    /*
     * (3) Uncommitted physical memory: it is committed and mapped.
     *
     * Guest physical memory is no longer committed in full at startup (see xmemory.cpp): it is
     * committed the first time it is touched. One 4 MB chunk per fault means at most 128 chunks, which
     * with the 360's five views are about 640 mappings (1 MB chunks gave about 2,500); with 64 KB pages
     * it would be 40,000 and the kernel runs out of blocks. The same instruction is retried, without touching the PC.
     */
    if (!hay_permiso) {
      constexpr size_t kFisicaIni = 0x100000000ull;
      constexpr size_t kFisicaFin = 0x120000000ull;
      constexpr size_t kTrozo = 0x400000;  // 4 MB: fewer chunks and fewer mappings
      size_t offset = 0;
      if (RexGmWindowToOffset(far, &offset) && offset >= kFisicaIni && offset < kFisicaFin) {
        if (RexGmCommit(offset & ~(kTrozo - 1), kTrozo, REX_GM_WRITE) && RexGmFaultIn(far)) {
          RexSwitchPerfCount(17);
          RexResumeFromException(ctx);
        }
      }
    }
  }
  // --- 1. read from a watched page: emulated through the shadow -------------
  //
  // On Switch there is no such thing as a read-only page: as soon as the GPU watches a
  // chunk of physical memory, that chunk is unmapped, and then every guest read of it
  // faults. And reading does not remove the watch, so it faults again, and again, as
  // long as the game keeps reading from there. Measured in a race: 83,000 emulated
  // reads per second, more than half a core.
  //
  // Treating the read as a write was tried, and it does not work. The cost went away
  // (83,000 faults/s dropped to 21, and CPU from 292% to 118%) but the game froze while
  // loading a race, with all its threads asleep waiting for something that never came.
  // The reason is in PhysicalHeap::TriggerCallbacks: reporting a write does not only
  // unwatch the page that faulted but up to 4 MB around it (unwatch_exact_range =
  // false), and it also tells the GPU caches that the range changed. With that, write
  // watching was left almost empty (the SDK handler's faults dropped from 240/s to
  // 11/s) and coherence between the guest and the GPU broke.
  //
  // So it is still emulated through the shadow, which is correct even if expensive.
  // This is now resolved above, in point (1) of the hot path, without asking again.
  // The reasoning stays written here because it explains why it cannot be done any
  // other way.

  // --- 2. SDK handlers ----------------------------------------------
  HostThreadContext tc;
  FillContext(tc, ctx);
  Exception ex;
  bool clasificado = true;
  if (data_abort) {
    ex.InitializeAccessViolation(&tc, far,
                                 is_write ? Exception::AccessViolationOperation::kWrite
                                          : Exception::AccessViolationOperation::kRead);
  } else if (ec == kEcUnknown) {
    ex.InitializeIllegalInstruction(&tc);
  } else {
    clasificado = false;
  }

  if (clasificado) {
    for (size_t i = 0; i < kMaxHandlerCount && handlers_[i].first; ++i) {
      if (!handlers_[i].first(&ex, handlers_[i].second)) {
        continue;
      }
      WriteBack(ex, tc, ctx);

      // Did it ask for a retry? Then the page must already be mapped, or the retry
      // would keep faulting forever.
      if (in_window && ctx->pc.x == pc) {
        size_t len = 0x1000;
        RexGmAccess a = REX_GM_NONE;
        const bool ok = RexGmQueryProtect(reinterpret_cast<uint8_t*>(far), &len, &a);
        // A read can be retried on REX_GM_READ: it will fault again, but it will land
        // in step 1 and be emulated. A write needs WRITE.
        const bool reintentable = ok && (is_write ? a == REX_GM_WRITE : a != REX_GM_NONE);
        if (!reintentable) {
          if (EmulateViaShadow(ctx)) {
            RexSwitchPerfCount(3);
            RexResumeFromException(ctx);
          }
          break;  // ni reintentable ni emulable: sigue a SEH y a fatal
        }
      }
      RexSwitchPerfCount(2);
      RexResumeFromException(ctx);
    }
  }

  // --- 3. SEH ----------------------------------------------------------------
  if (RexSwitchSehHandleFault(ctx)) {
    RexSwitchPerfCount(4);
    RexResumeFromException(ctx);
  }

  // --- 4. fatal --------------------------------------------------------------
  // The report goes to <NRO folder>/logs/rex/rex_crash.log. diagAbortWithResult is a system call,
  // not another exception: it ends with error 2345-0102.
  (void)far;
  (void)esr;
  RexSwitchCrashLog(data_abort ? "fallo de acceso a memoria" : "excepcion de CPU", ctx,
                    ctx->sp.x, pc);
  diagAbortWithResult(MAKERESULT(Module_Libnx, 102));
}

}  // namespace

void ExceptionHandler::Install(Handler fn, void* data) {
  // There is nothing to install in the system: __libnx_exception_handler exists
  // once linked. This only records the handler.
  for (size_t i = 0; i < kMaxHandlerCount; ++i) {
    if (!handlers_[i].first) {
      handlers_[i].first = fn;
      handlers_[i].second = data;
      return;
    }
  }
  assert_always("Too many exception handlers installed");
}

void ExceptionHandler::Uninstall(Handler fn, void* data) {
  for (size_t i = 0; i < kMaxHandlerCount; ++i) {
    if (handlers_[i].first == fn && handlers_[i].second == data) {
      for (; i < kMaxHandlerCount - 1; ++i) {
        handlers_[i] = handlers_[i + 1];
      }
      handlers_[i].first = nullptr;
      handlers_[i].second = nullptr;
      break;
    }
  }
}

}  // namespace rex::arch

/*
 * The libnx entry point.
 *
 * Careful when linking: in libnx this symbol is weak, and a weak reference does not pull an object
 * out of a static library. There is no problem here because it lives in the same object as
 * ExceptionHandler::Install, which the MMIO handler references strongly, so the whole object comes
 * in. If it is ever split out, it has to go directly into the executable, like runtime_switch.cpp.
 */
extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
  rex::arch::HandleSwitchException(ctx);
}

#endif  // REX_PLATFORM_SWITCH
