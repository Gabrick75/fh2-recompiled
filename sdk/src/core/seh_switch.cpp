/**
 * @file        rex/core/seh_switch.cpp
 * @brief       SEH interface (rex/platform/seh.h) for Horizon
 *
 * What SEH does in this SDK
 *
 * The generator wraps guest functions that have SEH scopes in
 * SEH_TRY / SEH_CATCH_ALL / SEH_RETHROW (include/rex/platform/exceptions.h).
 * Outside Windows that is a C++ try/catch with a SehGuard that turns on
 * seh_active(), and the SIGSEGV handler in seh_posix.cpp throws a SehException
 * from the fault itself.
 *
 * Why faults are not turned into exceptions here
 *
 * 1. On Horizon you cannot throw from the fault handler. It runs on a separate
 *    stack and there is no signal frame to unwind through; measured on the
 *    console, you cannot even return from it.
 *
 * 2. The alternative (diverting the thread to a trampoline that throws in a
 *    normal context) requires the faulting instruction to be covered by the
 *    compiler's unwind and call-site tables, which normally needs
 *    -fnon-call-exceptions. The generator's CMake template only adds
 *    asynchronous EH flags on Windows, so there is no guarantee that the catch
 *    would ever run: the likely outcome would be std::terminate.
 *
 * 3. NFSMW does not use it. generate_exception_handlers is false by default, and
 *    it is commented out in app/overrides.toml. With that off, the SEH macros are
 *    not emitted.
 *
 * So a fault inside an SEH scope is treated like any other unresolved fault: a
 * fatal stop with the register dump. It is recorded in the thread's SEH state in
 * case it helps diagnosis. Games that rely on SEH are not supported by this
 * implementation; point 2 is the part that is missing.
 */

#include <rex/platform.h>

#if REX_PLATFORM_SWITCH

#include <cstdlib>

#include <rex/platform/exceptions.h>
#include <rex/platform/seh.h>

#include <switch.h>

namespace rex::platform {

static thread_local SehThreadState tls_seh_state;
static thread_local bool tls_seh_active = false;

SehThreadState& seh_thread_state() {
  return tls_seh_state;
}

int seh_filter(u32 /*code*/, void* /*exception_pointers*/) {
  // Only used on Windows.
  return 0;
}

[[noreturn]] void seh_rethrow() {
  // On POSIX the signal is raised again with the default handler, which
  // terminates the process. There are no signals here: it terminates directly.
  std::abort();
}

void seh_initialize() {
  // Nothing to install: the only fault entry point on Horizon is
  // __libnx_exception_handler, in exception_handler_switch.cpp, and it asks
  // here through RexSwitchSehHandleFault.
  g_seh_initialized.store(true);
}

bool& seh_active() {
  return tls_seh_active;
}

}  // namespace rex::platform

/*
 * Called by exception_handler_switch.cpp when no SDK handler resolved the fault.
 * Returns true if it redirected execution; currently never, for the reasons given
 * in the file header.
 */
extern "C" bool RexSwitchSehHandleFault(ThreadExceptionDump* ctx) {
  if (!ctx || !rex::platform::seh_active()) {
    return false;
  }

  // It is recorded in the thread's SEH state, as seh_posix.cpp would do
  // before throwing: code and faulting address.
  const uint32_t ec = ctx->esr >> 26;
  const bool fallo_de_datos = (ec & 0b111110) == 0b100100;
  auto& estado = rex::platform::seh_thread_state();
  estado.code = fallo_de_datos ? rex::SehException::ACCESS_VIOLATION
                               : rex::SehException::ILLEGAL_INSTRUCTION;
  estado.info[0] = 0;
  estado.info[1] = static_cast<uintptr_t>(ctx->far.x);

  return false;
}

#endif  // REX_PLATFORM_SWITCH
