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
#include <ctime>
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
#include <rex/audio/xma/decoder.h>
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
extern std::atomic<float> g_fh2_luz_noche;

// The in-game "Test flags" panel (GameActivity.showFlagsDialog): sets one cvar while the game runs and says what happened.
// Returns "STATE|old value|new value" with STATE one of: LIVE (applied now), RESTART (init-only or restart-only flag: the
// panel saves it and it counts at the next start), TEXT (a text flag: not changed live, a renderer thread may be reading
// the string), UNKNOWN (no such flag), INVALID (the value was not accepted). LIVE means the game accepted the value; the
// few parts of the game that read a flag once at start keep using the old one.
extern "C" JNIEXPORT jstring JNICALL Java_com_forzahorizon2_android_GameActivity_nativeSetFlag(JNIEnv* env, jclass,
                                                                                            jstring jname,
                                                                                            jstring jvalue) {
  const auto to_string = [&](jstring text) {
    std::string out;
    if (const char* chars = text ? env->GetStringUTFChars(text, nullptr) : nullptr) {
      out = chars;
      env->ReleaseStringUTFChars(text, chars);
    }
    return out;
  };
  const std::string name = to_string(jname), value = to_string(jvalue);
  std::string state = "UNKNOWN", old_value, new_value;
  if (const rex::cvar::FlagEntry* info = name.empty() ? nullptr : rex::cvar::GetFlagInfo(name);
      info && info->type != rex::cvar::FlagType::Command) {
    const rex::cvar::Lifecycle lifecycle = info->lifecycle;
    const bool is_text = info->type == rex::cvar::FlagType::String;
    old_value = rex::cvar::GetFlagByName(name);
    if (lifecycle != rex::cvar::Lifecycle::kHotReload) {
      state = "RESTART";
    } else if (is_text) {
      state = "TEXT";
    } else if (rex::cvar::SetFlagByName(name, value)) {
      state = "LIVE";
      new_value = rex::cvar::GetFlagByName(name);
      if (name == "fh2_nativo_luz_noche") {  // the slider's value wins over the flag: keep them the same
        g_fh2_luz_noche.store(std::strtof(new_value.c_str(), nullptr), std::memory_order_relaxed);
      }
    } else {
      state = "INVALID";
    }
  }
  return env->NewStringUTF((state + "|" + old_value + "|" + new_value).c_str());
}

// The in-game "night light" slider (GameActivity.showSettings): the renderer reads it each time it fills the headlights'
// mask, so it takes effect at once.
extern "C" JNIEXPORT void JNICALL Java_com_forzahorizon2_android_GameActivity_nativeSetNightLight(JNIEnv*, jclass,
                                                                                               jfloat value) {
  g_fh2_luz_noche.store(value, std::memory_order_relaxed);
}

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

// GPU nanoseconds of the native renderer's measured submissions, accumulated (fh2_nativo_medir_gpu_uso or
// fh2_nativo_medir_gpu); -1 without timestamps. UsageMonitor turns it into "GPU %" where no kernel file is readable.
extern std::atomic<int64_t> g_fh2_nativo_gpu_ns;

extern "C" JNIEXPORT jlong JNICALL Java_com_forzahorizon2_android_UsageMonitor_nativeGpuNs(JNIEnv*, jclass) {
  return g_fh2_nativo_gpu_ns.load(std::memory_order_relaxed);
}

// Per-thread core pinning by name (adapted from NFSMW-Recompiled-Mobile, android/app/src/main/cpp/afinidad.cpp,
// where pinning the command processor and the game's main thread to the prime cores gave +17 %).
//
//   --fh2_android_afinidade="GPU Commands=7;*=3-6"
//
// Rules "name prefix=cores", first match wins, "*" for the rest; cores as "7", "3-6" or "3+5+6". A thread
// ("fh2 afinidade") walks /proc/self/task every 2 s and sets each thread's mask, so threads created later, and masks
// Android resets after a pause, are covered too. 'off' leaves everything to fh2_android_nucleos_grandes.
// 'jogo-rapido' is the dynamic mode below (busiest game thread alone on the fastest core).
REXCVAR_DEFINE_STRING(fh2_android_afinidade, "auto", "FH2",
                      "Android: pin threads to cores by name, 'GPU Commands=7;*=3-6'; 'auto' = the command "
                      "processor alone on the fastest core, the rest on the other fast cores; 'jogo-rapido' = "
                      "the busiest game thread alone on the fastest core, the ring on its own big core; 'off' = "
                      "no pinning")
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

namespace fh2_jogo_rapido {

// Starts the dynamic affinity loop; true when the loop took over (the cvar is consumed).
bool Iniciar();

}  // namespace fh2_jogo_rapido

extern "C" void fh2_android_afinidade_iniciar() {
  std::string text = REXCVAR_GET(fh2_android_afinidade);
  if (text == "jogo-rapido" || text == "jogo_rapido") {
    if (fh2_jogo_rapido::Iniciar()) return;
    return;  // already logged why there is nothing to pin (e.g. no slow cluster)
  }
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
// and how long each frame's work really took, for the threads that make it: the native ring thread and the game's
// busiest threads. On the S23 Ultra the native renderer's work comes in bursts (threads at 30-50 %, GPU 59 % busy),
// so the governors kept the big cores at 0.9-2.2 GHz of 3.36 and the GPU at 220 of 719 MHz; with the hints the
// system raises the clocks for a frame whose work misses its target. Looked up with dlsym: the app runs from API 26.
//
// The work reported is the CPU time the session's busiest thread used since the previous Swap, not the time between
// Swaps: with the FPS limit on, that period includes the wait for the vblank, so it never fell below the target and
// the system never saw that the clocks were too low. S23 Ultra logs of 2026-10-14: with the 30 FPS limit the ring
// took 20-29 us per draw against 11-15 us without it (the clock dropped), and the Swaps snapped to the 33/50/66 ms
// vblank grid. No GPU time is reported: what the renderer measures (submit to fence) is latency with overlapping
// submits (80-100 ms per 30 ms frame), not the GPU's busy time.
//
// The target is 85 % of the FPS limit's frame time (video_mode_refresh_rate is twice the limit,
// GameOptions.vblankRate), so a frame's work fits before its vblank; with the limit off (240 Hz) it aims at 60 FPS.
//
//   --fh2_android_adpf=false   turns it off
REXCVAR_DEFINE_BOOL(fh2_android_adpf, true, "FH2",
                    "Android: performance hints (ADPF) for the ring and the game's busiest threads, target from the "
                    "FPS limit")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

struct Adpf {
  using GetManager = void* (*)();
  using CreateSession = void* (*)(void*, const int32_t*, size_t, int64_t);
  using ReportActual = int (*)(void*, int64_t);
  using SetThreads = int (*)(void*, const pid_t*, size_t);
  using SetPreferPowerEfficiency = int (*)(void*, bool);
  GetManager get_manager = nullptr;
  CreateSession create_session = nullptr;
  ReportActual report_actual = nullptr;
  SetThreads set_threads = nullptr;
  SetPreferPowerEfficiency prefer_efficiency = nullptr;
  void* session = nullptr;
  int64_t target_ns = 0;
  std::vector<pid_t> tids;
  std::map<pid_t, int64_t> cpu_ns;  // each session thread's CPU time at the previous Swap
  std::mutex mutex;
  bool tried = false;
  uint64_t reports = 0;
};

Adpf& AdpfState() {
  static Adpf adpf;
  return adpf;
}

// 85 % of the FPS limit's frame time, at most 60 FPS (the limit off is a 240 Hz vblank).
int64_t AdpfTargetNs() {
  double refresh_hz = std::atof(rex::cvar::GetFlagByName("video_mode_refresh_rate").c_str());
  if (!(refresh_hz > 0.0)) refresh_hz = 60.0;
  return int64_t(0.85e9 / std::min(60.0, refresh_hz / 2.0));
}

// A thread's CPU time; any thread of this process (the kernel's MAKE_THREAD_CPUCLOCK with CPUCLOCK_SCHED).
int64_t ThreadCpuNs(pid_t tid) {
  const clockid_t clock = clockid_t((~unsigned(tid) << 3) | 6);
  timespec ts{};
  if (clock_gettime(clock, &ts) != 0) return -1;
  return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

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
      a.target_ns = AdpfTargetNs();
      a.session = a.create_session(a.get_manager(), chosen.data(), chosen.size(), a.target_ns);
      if (a.session && a.prefer_efficiency) a.prefer_efficiency(a.session, false);
      REXLOG_INFO("[adpf] sessao {} com {} threads (alvo {:.1f} ms; trabalho = CPU da thread mais ocupada)",
                  a.session ? "criada" : "NAO criada", chosen.size(), double(a.target_ns) / 1e6);
    } else if (a.set_threads) {
      a.set_threads(a.session, chosen.data(), chosen.size());
    }
    a.tids = chosen;
  }
}

}  // namespace

// Called by the native renderer at each Swap (app/src/nativo/nfsmw_nativo_destinos.cpp): the time since the previous
// Swap. The work reported is the CPU time of the session's busiest thread since the previous Swap.
extern "C" void fh2_android_fotograma(int64_t periodo_ns) {
  Adpf& a = AdpfState();
  if (!a.tried) {
    a.tried = true;
    if (!REXCVAR_GET(fh2_android_adpf)) {
      REXLOG_INFO("[adpf] desligado");
      return;
    }
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    a.get_manager = reinterpret_cast<Adpf::GetManager>(dlsym(lib, "APerformanceHint_getManager"));
    a.create_session = reinterpret_cast<Adpf::CreateSession>(dlsym(lib, "APerformanceHint_createSession"));
    a.report_actual = reinterpret_cast<Adpf::ReportActual>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    a.set_threads = reinterpret_cast<Adpf::SetThreads>(dlsym(lib, "APerformanceHint_setThreads"));
    a.prefer_efficiency =
        reinterpret_cast<Adpf::SetPreferPowerEfficiency>(dlsym(lib, "APerformanceHint_setPreferPowerEfficiency"));
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
  int64_t trabalho = 0;
  for (pid_t tid : a.tids) {
    const int64_t agora = ThreadCpuNs(tid);
    if (agora < 0) continue;
    const auto anterior = a.cpu_ns.find(tid);
    if (anterior != a.cpu_ns.end()) trabalho = std::max(trabalho, agora - anterior->second);
    a.cpu_ns[tid] = agora;
  }
  for (auto it = a.cpu_ns.begin(); it != a.cpu_ns.end();) {  // threads that left the session
    it = std::find(a.tids.begin(), a.tids.end(), it->first) == a.tids.end() ? a.cpu_ns.erase(it) : std::next(it);
  }
  if (trabalho <= 0) return;  // the first Swap after the session's threads changed
  trabalho = std::min(trabalho, periodo_ns);
  a.report_actual(a.session, trabalho);
  if (++a.reports == 300) {
    REXLOG_INFO("[adpf] 300 quadros relatados (ultimo: trabalho {:.1f} ms em {:.1f} ms, alvo {:.1f} ms)",
                double(trabalho) / 1e6, double(periodo_ns) / 1e6, double(a.target_ns) / 1e6);
  }
}

// 'jogo-rapido': the busiest game thread alone on the fastest core, the ring on its own big core.
//
// 'auto' pins the command processor (GPU Commands / GPU anillo nativo) on the fastest core, from when the ring
// was the bottleneck (+16 %). With the vertex cache the ring is ~20 % cheaper per draw and the game's own thread
// saturates first in heavy scenery (S23 Ultra: game thread at 90-97 %, GPU at 78-85 % with room on the ring), so
// the fastest core (X3) should go to whoever limits the frame rate instead. The busiest XThread changes between
// loads, so it is re-evaluated every 2 s (same cadence as AffinityLoop and the ADPF loop) with hysteresis: the
// pinned thread only moves when another game thread used clearly more CPU in the window, or when the pinned one
// stopped being the busiest twice in a row. Static thread names cannot be used: the main thread's name changes
// every run (XThreadXXXXXXXX), so busy-ness comes from /proc/self/task deltas, like AdpfThreadLoop does.
namespace fh2_jogo_rapido {

bool IsGameThread(const std::string& name) {
  // Kernel thread names (comm) are truncated to 15 chars: "Main XThread" stays readable, "XThreadABCD
  // (12345678)" becomes "XThreadABCD (12". Both are the game's threads (sdk/src/system/kernel_state.cpp and
  // xthread.cpp); the ring ("GPU anillo nat", "GPU Commands", "GPU VSync nativ") never matches these prefixes.
  return name.rfind("XThread", 0) == 0 || name.rfind("Main XThread", 0) == 0;
}

// The threads that keep the sound going: AAudio's callback thread ("AudioTrack", or "AAudio_N" on the MMAP path),
// SDL's playback thread ("SDLAudioP1"), the Audio Worker that runs the game's audio callback, the XMA Decoder and
// the SDL pump ("Audio bomba"). Each is light, but it has to run within its 5.3 ms block. Squeezed onto the 'rest'
// cores with every other game thread (two cores on a POCO X7 Pro), they ran late and the sound came out slow and
// robotic: ~1,355 blocks per 10 s instead of 1,875, against 1,875 with 'auto'.
bool IsAudioThread(const std::string& name) {
  return name.rfind("Audio", 0) == 0 || name.rfind("AAudio", 0) == 0 || name.rfind("SDLAudio", 0) == 0 ||
         name.rfind("XMA Decoder", 0) == 0;
}

bool IsRingThread(const std::string& name) {
  return name.rfind("GPU Commands", 0) == 0 || name.rfind("GPU anillo", 0) == 0 ||
         name.rfind("GPU VSync", 0) == 0;
}

void SetMask(pid_t tid, const cpu_set_t& mask) {
  cpu_set_t current;
  if (sched_getaffinity(tid, sizeof(current), &current) == 0 && !CPU_EQUAL(&current, &mask)) {
    sched_setaffinity(tid, sizeof(mask), &mask);
  }
}

void Loop(int fastest, cpu_set_t mask_fastest, cpu_set_t mask_ring, cpu_set_t mask_rest) {
  pthread_setname_np(pthread_self(), "fh2 jogo-rapido");
  // Audio threads, and the game's audio server (the thread sending the XMA kicks), get the ring's core as well
  // as the rest: every fast core but the one the busiest game thread has to itself.
  cpu_set_t mask_audio;
  CPU_OR(&mask_audio, &mask_ring, &mask_rest);
  std::map<pid_t, std::string> audio_avisados;
  auto before = ThreadTicks();
  pid_t elegido = -1;
  std::string elegido_nombre;
  int racha = 0;  // windows in a row where the pinned thread was not the busiest (below the margin)
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto now = ThreadTicks();
    // CPU ticks used in this window, by tid.
    pid_t ocupado = -1;
    std::string ocupado_nombre;
    unsigned long long ocupado_uso = 0;
    unsigned long long elegido_uso = 0;
    bool elegido_vive = false;
    for (const auto& [tid, info] : now) {
      if (!IsGameThread(info.first)) continue;
      const auto b = before.find(tid);
      const unsigned long long uso = b == before.end() ? 0 : info.second - b->second.second;
      if (tid == elegido) {
        elegido_vive = true;
        elegido_uso = uso;
      }
      if (uso > ocupado_uso) {
        ocupado_uso = uso;
        ocupado = tid;
        ocupado_nombre = info.first;
      }
    }
    before = now;
    if (ocupado >= 0 && ocupado_uso > 0) {
      if (elegido < 0 || !elegido_vive) {
        elegido = ocupado;
        elegido_nombre = ocupado_nombre;
        racha = 0;
        REXLOG_INFO("[afinidade] juego-rapido: '{}' (tid {}) al nucleo mas rapido ({}; +{} ticks)",
                    elegido_nombre, elegido, fastest, ocupado_uso);
      } else if (ocupado != elegido) {
        // Hysteresis: move right away on a clear win (+20 %), otherwise only after two windows behind, so
        // loads (where the busiest thread changes) settle instead of flapping every 2 s.
        if (ocupado_uso >= elegido_uso + elegido_uso / 5) {
          elegido = ocupado;
          elegido_nombre = ocupado_nombre;
          racha = 0;
          REXLOG_INFO("[afinidade] juego-rapido: '{}' (tid {}) al nucleo mas rapido ({}; +{} ticks contra {})",
                      elegido_nombre, elegido, fastest, ocupado_uso, elegido_uso);
        } else if (++racha >= 2) {
          elegido = ocupado;
          elegido_nombre = ocupado_nombre;
          racha = 0;
          REXLOG_INFO("[afinidade] juego-rapido: '{}' (tid {}) al nucleo mas rapido ({}; dos ventanas sin liderar)",
                      elegido_nombre, elegido, fastest);
        }
      } else {
        racha = 0;
      }
    }
    // Reapply every window: covers threads created later and masks Android resets after a pause.
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
      const pid_t tid = pid_t(std::atoi(entry.path().filename().c_str()));
      if (tid <= 0) continue;
      if (tid == elegido) {
        SetMask(tid, mask_fastest);
        continue;
      }
      std::ifstream comm(entry.path() / "comm");
      std::string name;
      std::getline(comm, name);
      if (IsAudioThread(name) || uint32_t(tid) == rex::audio::HiloDelUltimoKickXma()) {
        if (audio_avisados.emplace(tid, name).second) {
          REXLOG_INFO("[afinidade] juego-rapido: '{}' (tid {}) es de audio: nucleos del anillo y del resto", name,
                      tid);
        }
        SetMask(tid, mask_audio);
        continue;
      }
      SetMask(tid, IsRingThread(name) ? mask_ring : mask_rest);
    }
  }
}

bool Iniciar() {
  const int total = int(sysconf(_SC_NPROCESSORS_CONF));
  if (total <= 1 || total > CPU_SETSIZE) return false;
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
  // The ring gets its own big core (the highest-numbered other fast core); the rest share the remaining fast
  // cores. On the Moto G9 Play every core is within 70 % of the fastest, so the "big" set is all the others.
  int ring = -1;
  cpu_set_t mask_fastest, mask_ring, mask_rest;
  CPU_ZERO(&mask_fastest);
  CPU_ZERO(&mask_ring);
  CPU_ZERO(&mask_rest);
  bool any_slower = false;
  std::string resto;
  for (int cpu = 0; cpu < total; ++cpu) {
    if (khz[size_t(cpu)] < fastest_khz) any_slower = true;
    if (cpu != fastest && khz[size_t(cpu)] * 10 >= fastest_khz * 7) {
      if (cpu > ring) ring = cpu;
    }
  }
  if (fastest < 0 || !any_slower || ring < 0) {
    REXLOG_INFO("[afinidade] juego-rapido sin grupo lento: afinidad sin cambios");
    return false;
  }
  CPU_SET(fastest, &mask_fastest);
  CPU_SET(ring, &mask_ring);
  for (int cpu = 0; cpu < total; ++cpu) {
    if (cpu != fastest && cpu != ring && khz[size_t(cpu)] * 10 >= fastest_khz * 7) {
      CPU_SET(cpu, &mask_rest);
      resto += (resto.empty() ? "" : "+") + std::to_string(cpu);
    }
  }
  if (resto.empty()) {  // only two fast cores: the rest shares the ring's core
    mask_rest = mask_ring;
    resto = std::to_string(ring);
  }
  REXLOG_INFO("[afinidade] juego-rapido: hilo del juego en {}, anillo en {}, resto en {}", fastest, ring,
              resto.c_str());
  std::thread(Loop, fastest, mask_fastest, mask_ring, mask_rest).detach();
  return true;
}

}  // namespace fh2_jogo_rapido
