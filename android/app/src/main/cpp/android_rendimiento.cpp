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

extern "C" JNIEXPORT jfloat JNICALL Java_com_forzahorizon2_android_GameActivity_nativeFps(JNIEnv*, jclass) {
  static std::atomic<float>* fps = nullptr;
  auto* value = PluginCounter("rex_fps_guest", fps);
  return value ? value->load(std::memory_order_relaxed) : -1.0f;
}

extern "C" JNIEXPORT jfloat JNICALL Java_com_forzahorizon2_android_GameActivity_nativeFrameMs(JNIEnv*, jclass) {
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

// 'auto': the command processor (the bottleneck) alone on the core with the highest top clock, every other thread
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
  return "GPU Commands=" + std::to_string(fastest) + ";*=" + others;
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
