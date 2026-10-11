// Keeps the game off the phone's little cores.
//
// On the Switch the game gets three whole cores at a fixed clock. A phone mixes big and little cores, and the
// scheduler judges a thread by its recent load: the ring thread and the game thread wait for each other every
// frame, look half idle, and end up on a little core at a low clock. That is a lost frame each time.
//
// At startup, before the game creates its threads (they inherit the mask), every thread of the process is
// limited to the cores whose top clock is at least 70 % of the fastest one. SoCs without a slow cluster
// (Snapdragon 8 Elite: 4.32 and 3.53 GHz) keep every core.
#include <dlfcn.h>
#include <jni.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <pthread.h>
#include <chrono>
#include <thread>
#include <map>

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

// On-screen FPS counter (GameActivity.showFpsCounter): the GPU plugin publishes the game's FPS and frame time
// over the last half second (vulkan/command_processor.cpp, IssueSwap). Looked up once the plugin is loaded;
// -1 before that.
namespace {

std::atomic<float>* PluginCounter(const char* name, std::atomic<float>*& cache) {
  if (!cache) {
    void* plugin = dlopen("librexgpu-xenos.so", RTLD_NOW | RTLD_NOLOAD);
    if (!plugin) {
      return nullptr;
    }
    cache = static_cast<std::atomic<float>*>(dlsym(plugin, name));
  }
  return cache;
}

}  // namespace

// The native renderer (nfsmw_renderizador = nativo) does not load the GPU plugin: it counts its own Swaps
// (app/src/nativo/nfsmw_nativo_destinos.cpp).
extern std::atomic<float> g_fh2_nativo_fps;
extern std::atomic<float> g_fh2_nativo_ms;

extern "C" JNIEXPORT jfloat JNICALL Java_com_forzahorizon2_android_GameActivity_nativeFps(JNIEnv*, jclass) {
  if (const float nativo = g_fh2_nativo_fps.load(std::memory_order_relaxed); nativo >= 0.0f) {
    return nativo;
  }
  static std::atomic<float>* fps = nullptr;
  auto* value = PluginCounter("rex_fps_guest", fps);
  return value ? value->load(std::memory_order_relaxed) : -1.0f;
}

extern "C" JNIEXPORT jfloat JNICALL Java_com_forzahorizon2_android_GameActivity_nativeFrameMs(JNIEnv*, jclass) {
  if (const float nativo = g_fh2_nativo_ms.load(std::memory_order_relaxed); nativo >= 0.0f) {
    return nativo;
  }
  static std::atomic<float>* ms = nullptr;
  auto* value = PluginCounter("rex_frame_ms_guest", ms);
  return value ? value->load(std::memory_order_relaxed) : -1.0f;
}

// Per-thread core pinning by name (adapted from NFSMW-Recompiled-Mobile, android/app/src/main/cpp/afinidad.cpp,
// where pinning the command processor and the game's main thread to the prime cores gave +17 %).
//
//   --fh2_android_afinidade="GPU Commands=7;*=3-6"
//
// Rules "name prefix=cores", first match wins, "*" for the rest; cores as "7", "3-6" or "3+5+6". A thread
// ("fh2 afinidade") walks /proc/self/task every 2 s and sets each thread's mask, so threads created later, and masks
// Android resets after a pause, are covered too. 'off' leaves everything to fh2_android_nucleos_grandes.
REXCVAR_DEFINE_STRING(fh2_android_afinidade, "auto", "FH2",
                      "Android: pin threads to cores by name, 'GPU Commands=7;*=3-6'; 'auto' = the command "
                      "processor alone on the fastest core, the rest on the other fast cores; 'off' = no pinning")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

struct AffinityRule {
  std::string prefix;  // "" = any thread ("*")
  cpu_set_t cores;
};

bool ParseCores(const std::string& text, cpu_set_t& cores) {
  CPU_ZERO(&cores);
  size_t start = 0;
  bool any = false;
  while (start < text.size()) {
    size_t end = text.find('+', start);
    const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t dash = part.find('-');
    const int from = std::atoi(part.c_str());
    const int to = dash == std::string::npos ? from : std::atoi(part.c_str() + dash + 1);
    for (int cpu = from; cpu <= to && cpu < CPU_SETSIZE; ++cpu) {
      if (cpu >= 0) {
        CPU_SET(cpu, &cores);
        any = true;
      }
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return any;
}

std::vector<AffinityRule> ParseRules(const std::string& text) {
  std::vector<AffinityRule> rules;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(';', start);
    const std::string rule = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    const size_t eq = rule.rfind('=');
    if (eq != std::string::npos) {
      AffinityRule r;
      r.prefix = rule.substr(0, eq);
      if (r.prefix == "*") r.prefix.clear();
      if (ParseCores(rule.substr(eq + 1), r.cores)) rules.push_back(r);
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return rules;
}

void AffinityLoop(std::vector<AffinityRule> rules) {
  pthread_setname_np(pthread_self(), "fh2 afinidade");
  std::map<int, std::string> applied;  // tid -> rule prefix last applied
  while (true) {
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
      const int tid = std::atoi(entry.path().filename().c_str());
      std::ifstream comm(entry.path() / "comm");
      std::string name;
      std::getline(comm, name);
      for (const auto& rule : rules) {
        if (name.compare(0, rule.prefix.size(), rule.prefix) != 0) continue;
        cpu_set_t current;
        if (sched_getaffinity(tid, sizeof(current), &current) == 0 && !CPU_EQUAL(&current, &rule.cores)) {
          sched_setaffinity(tid, sizeof(rule.cores), &rule.cores);
          if (applied[tid] != rule.prefix || rule.prefix.size()) {
            if (!rule.prefix.empty()) {
              REXLOG_INFO("[afinidade] '{}' (tid {}) -> regra '{}'", name, tid, rule.prefix);
            }
            applied[tid] = rule.prefix;
          }
        }
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
}

}  // namespace

// 'auto': the command processor (the bottleneck) of either renderer alone on the core with the highest top clock, every other thread
// on the remaining cores whose top clock is at least 70 % of it. Measured on the S23 Ultra (Snapdragon 8 Gen 2, one
// 3.36 GHz X3 at cpu7 and four 2.8 GHz cores): event menu 18.5 -> 21.5 fps (+16 %). Nothing when all the cores run
// at the same top clock.
std::string AutoAffinityRules() {
  const int total = int(sysconf(_SC_NPROCESSORS_CONF));
  int fastest = -1;
  long fastest_khz = 0;
  std::vector<long> khz(size_t(std::max(total, 0)), 0);
  for (int cpu = 0; cpu < total; ++cpu) {
    khz[size_t(cpu)] = RelojMaximoKhz(cpu);
    if (khz[size_t(cpu)] > fastest_khz) {
      fastest_khz = khz[size_t(cpu)];
      fastest = cpu;
    }
  }
  std::string others;
  bool any_slower = false;
  for (int cpu = 0; cpu < total; ++cpu) {
    if (khz[size_t(cpu)] < fastest_khz) any_slower = true;
    if (cpu != fastest && khz[size_t(cpu)] * 10 >= fastest_khz * 7) {
      others += (others.empty() ? "" : "+") + std::to_string(cpu);
    }
  }
  if (fastest < 0 || !any_slower || others.empty()) return "";
  // The command processor is "GPU Commands" with the emulated GPU and "GPU anillo nativo" (the ring thread,
  // app/src/nativo/nfsmw_nativo_sistema.cpp) with the native renderer. Before the second rule the native mode left
  // the fastest core idle and put its ring thread with the game's threads on the other four.
  const std::string rapido = std::to_string(fastest);
  return "GPU Commands=" + rapido + ";GPU anillo=" + rapido + ";*=" + others;
}

extern "C" void fh2_android_afinidade_iniciar() {
  std::string text = REXCVAR_GET(fh2_android_afinidade);
  if (text == "auto") text = AutoAffinityRules();
  if (text.empty() || text == "off") return;
  auto rules = ParseRules(text);
  if (rules.empty()) {
    REXLOG_WARN("[afinidade] regras inválidas: '{}'", text);
    return;
  }
  REXLOG_INFO("[afinidade] {} regras: {}", rules.size(), text);
  std::thread(AffinityLoop, std::move(rules)).detach();
}

// ADPF: Android's performance hints (APerformanceHint, NDK, API 33+). The system is told the frame time we aim for
// (33.3 ms) and how long each frame really took, for the threads that make it: the native ring thread and the game's
// busiest threads. On the S23 Ultra the native renderer's work comes in bursts (threads at 30-50 %, GPU 59 % busy),
// so the governors kept the big cores at 0.9-2.2 GHz of 3.36 and the GPU at 220 of 719 MHz; with the hints the
// system raises the clocks for a frame that misses its target. From API 35 the report also carries the GPU time
// (AWorkDuration), which the system may use for the GPU's clock. Looked up with dlsym: the app runs from API 26.
//
//   --fh2_android_adpf=false   turns it off
REXCVAR_DEFINE_BOOL(fh2_android_adpf, true, "FH2",
                    "Android: performance hints (ADPF) for the ring and the game's busiest threads, 33.3 ms target")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

struct Adpf {
  using GetManager = void* (*)();
  using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
  using ReportActual = int (*)(void*, int64_t);
  using SetThreads = int (*)(void*, const pid_t*, size_t);
  using SetPreferPowerEfficiency = int (*)(void*, bool);
  using ReportActual2 = int (*)(void*, void*);
  using WorkCreate = void* (*)();
  using WorkRelease = void (*)(void*);
  using WorkSetI64 = void (*)(void*, int64_t);
  GetManager get_manager = nullptr;
  CreateSession create_session = nullptr;
  ReportActual report_actual = nullptr;
  SetThreads set_threads = nullptr;
  SetPreferPowerEfficiency prefer_efficiency = nullptr;
  ReportActual2 report_actual2 = nullptr;
  WorkCreate work_create = nullptr;
  WorkRelease work_release = nullptr;
  WorkSetI64 work_start = nullptr, work_total = nullptr, work_cpu = nullptr, work_gpu = nullptr;
  void* session = nullptr;
  std::vector<pid_t> tids;
  std::mutex mutex;
  bool tried = false;
  uint64_t reports = 0;
};

Adpf& AdpfState() {
  static Adpf adpf;
  return adpf;
}

constexpr int64_t kAdpfTargetNs = 33333333;

// CPU ticks of every thread, by name.
std::map<pid_t, std::pair<std::string, unsigned long long>> ThreadTicks() {
  std::map<pid_t, std::pair<std::string, unsigned long long>> out;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task")) {
    const pid_t tid = pid_t(std::atoi(entry.path().filename().c_str()));
    std::ifstream stat(entry.path() / "stat");
    std::string line;
    std::getline(stat, line);
    const size_t open = line.find('('), close = line.rfind(')');
    if (open == std::string::npos || close == std::string::npos) continue;
    std::istringstream fields(line.substr(close + 2));
    std::string field;
    unsigned long long utime = 0, stime = 0;
    for (int i = 3; i <= 15 && fields >> field; ++i) {
      if (i == 14) utime = std::stoull(field);
      if (i == 15) stime = std::stoull(field);
    }
    out[tid] = {line.substr(open + 1, close - open - 1), utime + stime};
  }
  return out;
}

// Every 2 s: the session's threads are the native ring thread and the three game threads (XThread*) that used the
// most CPU in the last 2 s.
void AdpfThreadLoop() {
  pthread_setname_np(pthread_self(), "fh2 adpf");
  auto before = ThreadTicks();
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto now = ThreadTicks();
    std::vector<std::pair<unsigned long long, pid_t>> game;
    std::vector<pid_t> chosen;
    for (const auto& [tid, info] : now) {
      const auto b = before.find(tid);
      const unsigned long long used = b == before.end() ? 0 : info.second - b->second.second;
      if (info.first.rfind("GPU anillo", 0) == 0 || info.first.rfind("GPU Commands", 0) == 0) {
        chosen.push_back(tid);
      } else if (info.first.rfind("XThread", 0) == 0 && used > 0) {
        game.emplace_back(used, tid);
      }
    }
    before = now;
    std::sort(game.rbegin(), game.rend());
    for (size_t i = 0; i < game.size() && i < 3; ++i) chosen.push_back(game[i].second);
    std::sort(chosen.begin(), chosen.end());
    Adpf& a = AdpfState();
    std::lock_guard<std::mutex> lock(a.mutex);
    if (chosen.empty() || chosen == a.tids) continue;
    if (!a.session) {
      a.session = a.create_session(a.get_manager(), chosen.data(), chosen.size(), kAdpfTargetNs);
      if (a.session && a.prefer_efficiency) a.prefer_efficiency(a.session, false);
      REXLOG_INFO("[adpf] sessao {} com {} threads (alvo 33,3 ms; GPU no relatorio: {})",
                  a.session ? "criada" : "NAO criada", chosen.size(), a.report_actual2 ? "sim" : "nao");
    } else if (a.set_threads) {
      a.set_threads(a.session, chosen.data(), chosen.size());
    }
    a.tids = chosen;
  }
}

}  // namespace

// Called by the native renderer at each Swap (app/src/nativo/nfsmw_nativo_destinos.cpp): the time since the previous
// Swap and the GPU time of that frame (0 if unknown).
extern "C" void fh2_android_fotograma(int64_t periodo_ns, int64_t gpu_ns) {
  Adpf& a = AdpfState();
  if (!a.tried) {
    a.tried = true;
    if (!REXCVAR_GET(fh2_android_adpf)) return;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    a.get_manager = reinterpret_cast<Adpf::GetManager>(dlsym(lib, "APerformanceHint_getManager"));
    a.create_session = reinterpret_cast<Adpf::CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
    a.report_actual = reinterpret_cast<Adpf::ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    a.set_threads = reinterpret_cast<Adpf::SetThreads>(dlsym(lib, "APerformanceHint_setThreads"));
    a.prefer_efficiency =
        reinterpret_cast<Adpf::SetPreferPowerEfficiency>(dlsym(lib, "APerformanceHint_setPreferPowerEfficiency"));
    a.report_actual2 = reinterpret_cast<Adpf::ReportActual2>(dlsym(lib, "APerformanceHint_reportActualWorkDuration2"));
    a.work_create = reinterpret_cast<Adpf::WorkCreate>(dlsym(lib, "AWorkDuration_create"));
    a.work_release = reinterpret_cast<Adpf::WorkRelease>(dlsym(lib, "AWorkDuration_release"));
    a.work_start = reinterpret_cast<Adpf::WorkSetI64>(dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos"));
    a.work_total = reinterpret_cast<Adpf::WorkSetI64>(dlsym(lib, "AWorkDuration_setActualTotalDurationNanos"));
    a.work_cpu = reinterpret_cast<Adpf::WorkSetI64>(dlsym(lib, "AWorkDuration_setActualCpuDurationNanos"));
    a.work_gpu = reinterpret_cast<Adpf::WorkSetI64>(dlsym(lib, "AWorkDuration_setActualGpuDurationNanos"));
    if (!a.work_create || !a.work_release || !a.work_start || !a.work_total || !a.work_cpu || !a.work_gpu) {
      a.report_actual2 = nullptr;
    }
    if (!a.get_manager || !a.create_session || !a.report_actual || !a.get_manager()) {
      REXLOG_INFO("[adpf] dicas de desempenho indisponiveis neste Android");
      a.get_manager = nullptr;
      return;
    }
    std::thread(AdpfThreadLoop).detach();
  }
  if (!a.get_manager || periodo_ns <= 0) return;
  std::lock_guard<std::mutex> lock(a.mutex);
  if (!a.session) return;
  if (a.report_actual2 && gpu_ns > 0) {
    void* work = a.work_create();
    const int64_t now = int64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch()).count());
    a.work_start(work, now - periodo_ns);
    a.work_total(work, periodo_ns);
    a.work_cpu(work, periodo_ns);
    a.work_gpu(work, gpu_ns);
    a.report_actual2(a.session, work);
    a.work_release(work);
  } else {
    a.report_actual(a.session, periodo_ns);
  }
  if (++a.reports == 300) {
    REXLOG_INFO("[adpf] 300 quadros relatados (ultimo {:.1f} ms, GPU {:.1f} ms)", double(periodo_ns) / 1e6,
                double(gpu_ns) / 1e6);
  }
}
