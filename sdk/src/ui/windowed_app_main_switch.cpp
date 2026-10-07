/**
 * @file        ui/windowed_app_main_switch.cpp
 * @brief       Entry point for windowed applications on Nintendo Switch
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include <cstdio>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_switch.h>

// Only the syscall header: switch.h drags typedefs that collide with rex.
extern "C" {
#include <switch/types.h>
// libnx runtime internals used for the exit below.
extern u32 __nx_applet_exit_mode;
NX_NORETURN void __libnx_exit(int rc);
}

namespace {

// Returning from main is not a clean exit for this runtime on Switch. libnx
// never runs .fini_array, so static destructors do not run, and any thread
// owned by a global object (the TimerQueue dispatcher, and in a title the
// guest, GPU and audio threads) outlives the NRO. hbloader then loads hbmenu
// into the same process while those threads still own stacks aliased out of
// the heap, and hbmenu data-aborts on the first heap page it touches (seen on
// console with uiprobe; the Atmosphere report showed the TimerQueue thread
// still waiting in TimerThreadMain). The process has to end instead, which
// takes every thread down with it.
//
// Ending it directly is not right either: svcExitProcess, with or without
// __appExit first, is reported by the system as "closed because an error
// occurred", with no crash report (seen on console). An application must ask
// the system to close it. libnx does that when __nx_applet_exit_mode is 1: its
// applet cleanup installs _appletExitProcess as the exit function, which sends
// ISelfController::Exit and sleeps until the system terminates the process
// (read from libnx's _appletCleanup in the linked binary). __libnx_exit runs
// that path after closing the services, without returning to hbloader.
[[noreturn]] void ExitProcess(int result) {
  REXLOG_INFO("Switch: exiting process ({})", result);
  rex::FlushLogging();
  std::fflush(nullptr);
  __nx_applet_exit_mode = 1;
  __libnx_exit(result);
}

}  // namespace

int main(int argc, char* argv[]) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result;
  {
    rex::ui::SwitchWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      ExitProcess(EXIT_FAILURE);
    }

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    const bool initialized = app->OnInitialize();
    if (!initialized) {
      REXLOG_ERROR("Switch: app initialization failed");
    }
    result = initialized ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

    app->InvokeOnDestroy();
  }
  ExitProcess(result);
}
