/**
 * @file        rex/core/fiber_switch.cpp
 * @brief       AAPCS64 rex::thread::Fiber backend (no ucontext)
 *
 * Horizon and Android's supported NDK do not provide makecontext/swapcontext,
 * so the context switch is done by hand in AArch64 assembly.
 *
 * It is less daunting than it sounds. A cooperative switch only has to preserve what
 * AAPCS64 requires the callee to preserve:
 *
 *   x19-x28   integers
 *   x29 (fp)  frame pointer
 *   x30 (lr)  return address; it is where the final ret jumps to
 *   sp        stack
 *   d8-d15    floating point, only the low 64 bits
 *
 * Everything volatile (x0-x18, d0-d7, d16-d31) has already been saved by the compiler
 * before calling SwitchTo, because for it this is a normal call.
 *
 * What is deliberately not touched:
 *
 *   TPIDR_EL0  the TLS register. Fibers of the same thread share thread-local
 *              storage, which is exactly what the SDK wants: tls_current_ must
 *              remain the thread's.
 *
 * About stack unwinding: the new fiber starts with fp and lr set to zero, so anything
 * that tries to unwind from its base stops there cleanly instead of following garbage.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH || (REX_PLATFORM_ANDROID && REX_ARCH_ARM64)

#include <rex/thread/fiber.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>

namespace {

using Ctx = rex::thread::Fiber::Context;

// The assembly below indexes by fixed offset. If someone reorders Context, this
// fails at compile time instead of corrupting the stack at run time.
static_assert(offsetof(Ctx, x19) == 0, "Context: x19 movido");
static_assert(offsetof(Ctx, x21) == 16, "Context: x21 movido");
static_assert(offsetof(Ctx, x23) == 32, "Context: x23 movido");
static_assert(offsetof(Ctx, x25) == 48, "Context: x25 movido");
static_assert(offsetof(Ctx, x27) == 64, "Context: x27 movido");
static_assert(offsetof(Ctx, fp) == 80, "Context: fp movido");
static_assert(offsetof(Ctx, lr) == 88, "Context: lr movido");
static_assert(offsetof(Ctx, sp) == 96, "Context: sp movido");
static_assert(offsetof(Ctx, d8) == 104, "Context: d8 movido");
static_assert(offsetof(Ctx, d10) == 120, "Context: d10 movido");
static_assert(offsetof(Ctx, d12) == 136, "Context: d12 movido");
static_assert(offsetof(Ctx, d14) == 152, "Context: d14 movido");
static_assert(sizeof(Ctx) == 168, "Context: tamano cambiado");

}  // namespace

extern "C" {

/// Saves the caller's context in *from and resumes *to.
/// Returns when someone switches back to *from.
void rex_fiber_swap(Ctx* from, Ctx* to);

/// First step of a new fiber. It zeroes fp and lr to cut unwinding and jumps
/// (without bl, so lr is not dirtied again) to the C++ trampoline.
void rex_fiber_entry(void);

// rex_fiber_trampoline is declared in fiber.h, so it can be a friend of Fiber.
}

__asm__(
    ".text\n"
    ".balign 4\n"
    ".global rex_fiber_swap\n"
    ".type rex_fiber_swap, %function\n"
    "rex_fiber_swap:\n"
    "    stp x19, x20, [x0, #0]\n"
    "    stp x21, x22, [x0, #16]\n"
    "    stp x23, x24, [x0, #32]\n"
    "    stp x25, x26, [x0, #48]\n"
    "    stp x27, x28, [x0, #64]\n"
    "    stp x29, x30, [x0, #80]\n"
    "    mov x2, sp\n"
    "    str x2, [x0, #96]\n"
    "    stp d8,  d9,  [x0, #104]\n"
    "    stp d10, d11, [x0, #120]\n"
    "    stp d12, d13, [x0, #136]\n"
    "    stp d14, d15, [x0, #152]\n"
    "\n"
    "    ldp x19, x20, [x1, #0]\n"
    "    ldp x21, x22, [x1, #16]\n"
    "    ldp x23, x24, [x1, #32]\n"
    "    ldp x25, x26, [x1, #48]\n"
    "    ldp x27, x28, [x1, #64]\n"
    "    ldp x29, x30, [x1, #80]\n"
    "    ldr x2, [x1, #96]\n"
    "    mov sp, x2\n"
    "    ldp d8,  d9,  [x1, #104]\n"
    "    ldp d10, d11, [x1, #120]\n"
    "    ldp d12, d13, [x1, #136]\n"
    "    ldp d14, d15, [x1, #152]\n"
    "    ret\n"
    ".size rex_fiber_swap, .-rex_fiber_swap\n"
    "\n"
    ".balign 4\n"
    ".global rex_fiber_entry\n"
    ".type rex_fiber_entry, %function\n"
    "rex_fiber_entry:\n"
    "    mov x29, #0\n"
    "    mov x30, #0\n"
    "    b   rex_fiber_trampoline\n"
    ".size rex_fiber_entry, .-rex_fiber_entry\n");

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  // Nothing to capture: the thread's context is saved by itself the first
  // time SwitchTo is called from it.
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  // AAPCS64 requires sp aligned to 16 on function entry. Another 16 bytes are
  // left free below the top as a cushion.
  auto top = reinterpret_cast<uintptr_t>(f->stack_.data()) + f->stack_.size();
  top &= ~uintptr_t(15);
  top -= 16;

  f->context_.sp = top;
  f->context_.lr = reinterpret_cast<uintptr_t>(&rex_fiber_entry);
  f->context_.fp = 0;
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_swap(&from->context_, &target->context_);
}

void Fiber::Destroy() {
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy sobre la fibra que esta corriendo");
  }
  delete this;
}

}  // namespace rex::thread

extern "C" [[noreturn]] void rex_fiber_trampoline(void) {
  // SwitchTo set tls_current_ before jumping here, just as the POSIX backend
  // does with its Trampoline.
  rex::thread::Fiber* f = rex::thread::Fiber::Current();
  f->entry_(f->arg_);
  // entry_ must not return: there is no context to go back to, this fiber's
  // stack ends here.
  assert(false && "la funcion de entrada de una fibra ha vuelto");
  std::abort();
}

#endif  // REX_PLATFORM_SWITCH || Android ARM64
