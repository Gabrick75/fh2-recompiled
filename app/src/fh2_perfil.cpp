// fh2 - in-process sampling profiler ("perfil" means "profile") for one thread, for machines where perf or
// simpleperf cannot be used (perf_event_paranoid on the PC).
//
//   --fh2_perfil_s=70 --fh2_perfil_thread="GPU Commands" --fh2_perfil_duracao_s=10
//
// At fh2_perfil_s seconds after start it finds the thread whose name starts with fh2_perfil_thread, sends it SIGPROF
// every millisecond (a POSIX timer aimed at that thread) and records the interrupted program counter. After
// fh2_perfil_duracao_s seconds it writes fh2_perfil.txt next to the log: one line per address, "count library
// offset", most frequent first. Symbolize with tools/perfil_simbolos.py (addr2line).

#include <dlfcn.h>
#include <signal.h>
#include <unwind.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_DOUBLE(fh2_perfil_s, 0.0, "FH2",
                      "Debug: start sampling one thread this many seconds after start (0 = off); app/src/fh2_perfil.cpp")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(fh2_perfil_duracao_s, 10.0, "FH2", "Debug: how long the sampling profiler runs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(fh2_perfil_thread, "GPU Commands", "FH2",
                      "Debug: the sampled thread, by the start of its name")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

constexpr size_t kMaxSamples = 1 << 20;
uintptr_t g_samples[kMaxSamples];
// The return address at the moment of the sample: [sp] on x86-64 (right for leaf functions such as memcpy, which
// push nothing), the link register on aarch64. For "who calls memcpy".
uintptr_t g_callers[kMaxSamples];
std::atomic<size_t> g_sample_count{0};
// Short stacks of the first kPilas samples (backtrace from the signal handler: diagnostics only), to find who calls
// into the dynamic loader or libc.
constexpr size_t kPilas = 32768, kMarcos = 12;
void* g_pilas[kPilas][kMarcos];
int g_marcos[kPilas];

// backtrace() is not in Android's libc before API 33: the same with the unwinder.
struct Pila {
  void** marcos;
  int maximo, n;
};
_Unwind_Reason_Code AnotarMarco(_Unwind_Context* contexto, void* datos) {
  auto* pila = static_cast<Pila*>(datos);
  if (pila->n >= pila->maximo) return _URC_END_OF_STACK;
  pila->marcos[pila->n++] = reinterpret_cast<void*>(_Unwind_GetIP(contexto));
  return _URC_NO_REASON;
}
int Pilas(void** marcos, int maximo) {
  Pila pila{marcos, maximo, 0};
  _Unwind_Backtrace(&AnotarMarco, &pila);
  return pila.n;
}

void OnSample(int, siginfo_t*, void* context) {
  auto* uc = static_cast<ucontext_t*>(context);
#if defined(__x86_64__)
  const uintptr_t pc = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
  const uintptr_t caller = *reinterpret_cast<const uintptr_t*>(uc->uc_mcontext.gregs[REG_RSP]);
#elif defined(__aarch64__)
  const uintptr_t pc = uintptr_t(uc->uc_mcontext.pc);
  const uintptr_t caller = uintptr_t(uc->uc_mcontext.regs[30]);
#else
  const uintptr_t pc = 0;
  const uintptr_t caller = 0;
#endif
  const size_t i = g_sample_count.fetch_add(1, std::memory_order_relaxed);
  if (i < kMaxSamples) {
    g_samples[i] = pc;
    g_callers[i] = caller;
  }
  if (i < kPilas) {
    g_marcos[i] = Pilas(g_pilas[i], int(kMarcos));
  }
}

// CPU ticks (utime + stime) of every thread of the process.
std::map<pid_t, unsigned long long> ThreadTicks() {
  std::map<pid_t, unsigned long long> ticks;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
    std::ifstream stat(entry.path() / "stat");
    std::string line;
    std::getline(stat, line);
    const size_t end = line.rfind(')');  // the name may hold spaces
    if (end == std::string::npos) continue;
    std::istringstream fields(line.substr(end + 2));
    std::string field;
    unsigned long long utime = 0, stime = 0;
    for (int i = 3; i <= 15 && fields >> field; ++i) {
      if (i == 14) utime = std::stoull(field);
      if (i == 15) stime = std::stoull(field);
    }
    ticks[pid_t(std::stoi(entry.path().filename().string()))] = utime + stime;
  }
  return ticks;
}

pid_t FindThread(const std::string& prefix) {
  if (prefix == "*ocupada") {
    // The thread that used the most CPU over one second (the game's main thread has a different XThread name
    // every run).
    const auto antes = ThreadTicks();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto despues = ThreadTicks();
    pid_t mejor = 0;
    unsigned long long maximo = 0;
    for (const auto& [tid, t] : despues) {
      const auto a = antes.find(tid);
      const unsigned long long uso = a == antes.end() ? 0 : t - a->second;
      if (uso > maximo && tid != pid_t(syscall(SYS_gettid))) {
        maximo = uso;
        mejor = tid;
      }
    }
    return mejor;
  }
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
    std::ifstream comm(entry.path() / "comm");
    std::string name;
    std::getline(comm, name);
    if (name.compare(0, prefix.size(), prefix) == 0) {
      return pid_t(std::stoi(entry.path().filename().string()));
    }
  }
  return 0;
}

void Run() {
  std::this_thread::sleep_for(std::chrono::duration<double>(REXCVAR_GET(fh2_perfil_s)));
  const std::string prefix = REXCVAR_GET(fh2_perfil_thread);
  const pid_t tid = FindThread(prefix);
  if (!tid) {
    REXLOG_WARN("[perfil] thread '{}' not found", prefix);
    return;
  }
  struct sigaction action {};
  action.sa_sigaction = OnSample;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(SIGPROF, &action, nullptr);

  sigevent event{};
  event.sigev_notify = SIGEV_THREAD_ID;
  event.sigev_signo = SIGPROF;
#if defined(__GLIBC__)
  event._sigev_un._tid = tid;
#else
  event.sigev_notify_thread_id = tid;
#endif
  timer_t timer;
  if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0) {
    REXLOG_WARN("[perfil] timer_create failed");
    return;
  }
  {
    void* calentar[2];
    Pilas(calentar, 2);  // the first call loads libgcc: not inside the signal handler
  }
  std::string nombre;
  {
    std::ifstream comm("/proc/self/task/" + std::to_string(tid) + "/comm");
    std::getline(comm, nombre);
  }
  REXLOG_INFO("[perfil] thread {} is '{}'", tid, nombre);
  itimerspec period{{0, 1000000}, {0, 1000000}};
  timer_settime(timer, 0, &period, nullptr);
  REXLOG_INFO("[perfil] sampling thread {} ('{}') for {} s", tid, prefix, REXCVAR_GET(fh2_perfil_duracao_s));
  std::this_thread::sleep_for(std::chrono::duration<double>(REXCVAR_GET(fh2_perfil_duracao_s)));
  timer_delete(timer);

  const size_t count = std::min(g_sample_count.load(), kMaxSamples);
  std::map<uintptr_t, uint32_t> histogram;
  for (size_t i = 0; i < count; ++i) {
    ++histogram[g_samples[i]];
  }
  std::vector<std::pair<uint32_t, uintptr_t>> sorted;
  for (const auto& [pc, n] : histogram) {
    sorted.emplace_back(n, pc);
  }
  std::sort(sorted.rbegin(), sorted.rend());
  FILE* out = fopen("fh2_perfil.txt", "w");
  if (!out) {
    return;
  }
  fprintf(out, "# %zu samples of thread '%s'\n", count, prefix.c_str());
  for (const auto& [n, pc] : sorted) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
      fprintf(out, "%u %s 0x%zx\n", n, info.dli_fname, size_t(pc - uintptr_t(info.dli_fbase)));
    } else {
      fprintf(out, "%u ? 0x%zx\n", n, size_t(pc));
    }
  }
  fclose(out);
  // For the samples in the dynamic loader, libc or libgcc: the first frame of the stack outside them.
  {
    const auto fuera = [](void* a, Dl_info& info) {
      if (!dladdr(a, &info) || !info.dli_fname) return true;
      const std::string f(info.dli_fname);
      return f.find("ld-linux") == std::string::npos && f.find("libc.so") == std::string::npos &&
             f.find("libgcc") == std::string::npos && f.find("libstdc++") == std::string::npos;
    };
    std::map<std::pair<std::string, size_t>, uint32_t> llamantes;
    const size_t con_pila = std::min(count, kPilas);
    for (size_t i = 0; i < con_pila; ++i) {
      Dl_info info{};
      if (fuera(reinterpret_cast<void*>(g_samples[i]), info)) continue;
      for (int m = 1; m < g_marcos[i]; ++m) {
        Dl_info f{};
        if (fuera(g_pilas[i][m], f) && f.dli_fname) {
          ++llamantes[{f.dli_fname, size_t(uintptr_t(g_pilas[i][m]) - uintptr_t(f.dli_fbase))}];
          break;
        }
      }
    }
    std::vector<std::pair<uint32_t, std::pair<std::string, size_t>>> orden;
    for (const auto& [k, n] : llamantes) orden.emplace_back(n, k);
    std::sort(orden.rbegin(), orden.rend());
    if (FILE* f = fopen("fh2_perfil_pilas.txt", "w")) {
      fprintf(f, "# first caller outside ld.so/libc/libgcc/libstdc++ of those samples (of the first %zu)\n", con_pila);
      for (const auto& [n, k] : orden) fprintf(f, "%u %s 0x%zx\n", n, k.first.c_str(), k.second);
      fclose(f);
    }
  }
  // Callers of the samples that fell in libc (memcpy, memmove, memset...): "count library offset".
  std::map<uintptr_t, uint32_t> callers;
  for (size_t i = 0; i < count; ++i) {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(g_samples[i]), &info) && info.dli_fname &&
        std::string(info.dli_fname).find("libc.so") != std::string::npos) {
      ++callers[g_callers[i]];
    }
  }
  std::vector<std::pair<uint32_t, uintptr_t>> sorted_callers;
  for (const auto& [pc, n] : callers) {
    sorted_callers.emplace_back(n, pc);
  }
  std::sort(sorted_callers.rbegin(), sorted_callers.rend());
  if (FILE* calls = fopen("fh2_perfil_libc.txt", "w")) {
    fprintf(calls, "# callers of the %zu-sample profile's libc samples\n", count);
    for (const auto& [n, pc] : sorted_callers) {
      Dl_info info{};
      if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
        fprintf(calls, "%u %s 0x%zx\n", n, info.dli_fname, size_t(pc - uintptr_t(info.dli_fbase)));
      } else {
        fprintf(calls, "%u ? 0x%zx\n", n, size_t(pc));
      }
    }
    fclose(calls);
  }
  REXLOG_INFO("[perfil] {} samples written to fh2_perfil.txt (libc callers: fh2_perfil_libc.txt)", count);
}

}  // namespace

// Called from OnPostSetup (fh2_app.h).
void Fh2PerfilStart() {
  if (REXCVAR_GET(fh2_perfil_s) > 0.0) {
    std::thread(Run).detach();
  }
}
