// Keeps the game off the phone's little cores.
//
// On the Switch the game gets three whole cores at a fixed clock. A phone mixes big and little cores, and the
// scheduler judges a thread by its recent load: the ring thread and the game thread wait for each other every
// frame, look half idle, and end up on a little core at a low clock. That is a lost frame each time.
//
// At startup, before the game creates its threads (they inherit the mask), every thread of the process is
// limited to the cores whose top clock is at least 70 % of the fastest one. SoCs without a slow cluster
// (Snapdragon 8 Elite: 4.32 and 3.53 GHz) keep every core.
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(fh2_android_nucleos_grandes, true, "FH2",
                    "Android: el juego solo usa los nucleos rapidos (reloj maximo de al menos el 70 % del mas "
                    "rapido); false = los que decida el sistema")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

long RelojMaximoKhz(int cpu) {
  std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq");
  long khz = 0;
  return (f >> khz) ? khz : 0;
}

}  // namespace

extern "C" void fh2_android_nucleos_grandes_aplicar() {
  if (!REXCVAR_GET(fh2_android_nucleos_grandes)) {
    return;
  }
  const int total = int(sysconf(_SC_NPROCESSORS_CONF));
  if (total <= 1 || total > CPU_SETSIZE) {
    return;
  }
  std::vector<long> relojes(size_t(total), 0);
  long maximo = 0;
  for (int cpu = 0; cpu < total; ++cpu) {
    relojes[size_t(cpu)] = RelojMaximoKhz(cpu);
    maximo = std::max(maximo, relojes[size_t(cpu)]);
  }
  if (maximo <= 0) {
    REXLOG_INFO("[android] no se pueden leer los relojes de los nucleos: afinidad sin cambios");
    return;
  }
  cpu_set_t mascara;
  CPU_ZERO(&mascara);
  int elegidos = 0;
  std::string lista;
  for (int cpu = 0; cpu < total; ++cpu) {
    if (relojes[size_t(cpu)] * 10 >= maximo * 7) {
      CPU_SET(cpu, &mascara);
      ++elegidos;
      lista += (lista.empty() ? "" : ",") + std::to_string(cpu);
    }
  }
  if (elegidos == total || elegidos < 2) {
    REXLOG_INFO("[android] {} nucleos sin grupo lento: afinidad sin cambios", total);
    return;
  }
  // Every thread that already exists (the SDL thread, the UI thread, the logger); the ones created later
  // inherit the mask of their creator.
  int hilos = 0;
  std::error_code ec;
  for (const auto& tarea : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    const pid_t tid = pid_t(std::strtol(tarea.path().filename().c_str(), nullptr, 10));
    if (tid > 0 && sched_setaffinity(tid, sizeof(mascara), &mascara) == 0) {
      ++hilos;
    }
  }
  REXLOG_INFO("[android] juego en los nucleos {} de {} (reloj >= 70 % de {} MHz); {} hilos movidos", lista,
              total, maximo / 1000, hilos);
}
