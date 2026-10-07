/**
 * @file        rex/thread/fiber.h
 * @brief       Host OS fiber primitive for cooperative context switching
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <rex/platform.h>
#include <cstddef>

// Switch and Android ARM64 use the AAPCS64 assembly backend. Switch goes
// before Linux because platform.h also enables the Linux compatibility layer.
#if REX_PLATFORM_SWITCH || (REX_PLATFORM_ANDROID && REX_ARCH_ARM64)
#include <cstdint>
#include <vector>
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#if REX_PLATFORM_MAC && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext APIs unless _XOPEN_SOURCE is defined
// before including <ucontext.h>.
#define _XOPEN_SOURCE 700
#endif
#include <ucontext.h>
#include <cstdint>
#include <vector>
#endif

#if REX_PLATFORM_SWITCH || (REX_PLATFORM_ANDROID && REX_ARCH_ARM64)
/// First C++ code that runs on a new AAPCS64 fiber. It is called by the
/// assembly trampoline in fiber_switch.cpp, and never returns.
extern "C" [[noreturn]] void rex_fiber_trampoline(void);
#endif

namespace rex::thread {

/// Host OS fiber primitive.
/// Each guest fiber gets one Fiber. Switching preserves the entire C++ call
/// stack, so mid-function resume works without any LR lookup.
struct Fiber {
  /// Convert the calling thread into a fiber.
  /// Must be called once on a thread before any SwitchTo.
  /// Returns a handle for the thread's current execution context.
  static Fiber* ConvertCurrentThread();

  /// Create a new fiber with its own host stack.
  /// entry(arg) is called when the fiber is first switched to.
  static Fiber* Create(size_t stack_size, void (*entry)(void*), void* arg);

  /// Suspend the current fiber and resume target.
  /// Returns when another fiber calls SwitchTo back to this one.
  static void SwitchTo(Fiber* target);

  /// Destroy this fiber. Must not be called while it is executing.
  void Destroy();

  /// Returns the fiber currently executing on this thread, or nullptr.
  static Fiber* Current() { return tls_current_; }

 private:
  static thread_local Fiber* tls_current_;

#if REX_PLATFORM_WIN32
  void* handle_ = nullptr;
  bool is_thread_fiber_ = false;
#elif REX_PLATFORM_SWITCH || (REX_PLATFORM_ANDROID && REX_ARCH_ARM64)
 public:
  /// Registers AAPCS64 requires the callee to preserve. They are all that needs saving in a
  /// cooperative context switch: the compiler has already saved the volatile ones before calling
  /// SwitchTo.
  ///
  /// The order matters: fiber_switch.cpp reads it from assembly at fixed offsets, and checks with
  /// static_assert that it has not moved.
  struct Context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp;   // x29
    uint64_t lr;   // x30, where ret jumps to
    uint64_t sp;
    uint64_t d8, d9, d10, d11, d12, d13, d14, d15;  // only the low 64 bits
  };

 private:
  Context context_{};
  std::vector<uint8_t> stack_;
  void (*entry_)(void*) = nullptr;
  void* arg_ = nullptr;
  bool is_thread_fiber_ = false;

  friend void ::rex_fiber_trampoline(void);
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
  ucontext_t context_{};
  std::vector<uint8_t> stack_;
  void (*entry_)(void*) = nullptr;
  void* arg_ = nullptr;
  bool is_thread_fiber_ = false;

  static void Trampoline();
#endif
};

}  // namespace rex::thread
