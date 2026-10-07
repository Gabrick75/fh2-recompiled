/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <atomic>
#include <chrono>

#include <rex/cvar.h>
#include <rex/filesystem/device.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/host_path_file.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/system/info/file.h>
#include <rex/system/kernel_state.h>
#include <rex/system/util/string_utils.h>
#include <rex/system/xevent.h>
#include <rex/system/xfile.h>
#include <rex/system/xiocompletion.h>
#include <rex/system/xsymboliclink.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread/mutex.h>

#include <cctype>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/*
 * I/O measurement.
 *
 * The log only showed failures (they are WARN); everything else (how many opens there are, how long
 * each one takes, how much is read) went to TRACE and did not show. And that is where the time was:
 * failing opens cost 32 ms each. So every open and every read is timed, the ones over the limit are
 * reported, and a summary is written every few seconds.
 *
 * A monotonic clock read per call is a few nanoseconds next to a trip to the SD. It is on.
 */
REXCVAR_DEFINE_INT32(nfsmw_io_aviso_ms, 8, "Filesystem",
                     "Avisa en el log de cada apertura o lectura que pase de estos ms (0 = nunca).");
REXCVAR_DEFINE_INT32(nfsmw_io_resumen_s, 15, "Filesystem",
                     "Cada cuantos segundos se escribe el resumen [io] (0 = nunca).");

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

namespace {

struct ContadoresIo {
  std::atomic<uint64_t> aperturas_ok{0};
  std::atomic<uint64_t> aperturas_fallo{0};
  std::atomic<uint64_t> us_aperturas{0};
  std::atomic<uint64_t> us_apertura_peor{0};
  std::atomic<uint64_t> lecturas{0};
  std::atomic<uint64_t> bytes_leidos{0};
  std::atomic<uint64_t> us_lecturas{0};
  std::atomic<uint64_t> us_lectura_peor{0};
};

ContadoresIo g_io;
std::atomic<uint32_t> g_io_avisos{0};
std::atomic<uint64_t> g_io_proximo_resumen{0};

/*
 * Which part of what is read had already been read, and whether it is read sequentially.
 *
 * Logging only how many bytes and from which file, not from where, the most that could be checked
 * was whether the (file, size) pair repeated, and it did: 270.2 MB of the 364.1 MB of slow reads
 * were repeats, and ZZDATA6.BIN repeated the same sizes every ~90 s, which is one lap of the
 * circuit. But "same size" is not "same place": the same size also comes from a sequential sweep
 * (the 31 consecutive 4.00 MB reads at startup are exactly that). Without the offset the two cannot
 * be told apart, and whether a block cache would remove reads depends on it.
 *
 * So the exact range is recorded. Two fixed-size tables, with no allocation or growth:
 *  - ranges already seen (8,192 slots of 8 bytes = 64 KB), to count rereads of the same place;
 *  - end of the last read of each file (64 slots), to count the reads that start exactly where the
 *    previous one ended, which is what tells whether the read-ahead window has anything to do.
 *
 * It costs a hash of the name and a lock per read. That is 1,290 reads in 435 s: nothing.
 */
struct EstadoRangos {
  static constexpr size_t kRanuras = 8192;  // power of two
  static constexpr size_t kFicheros = 64;
  static constexpr size_t kSondeos = 8;

  std::mutex mutex;
  uint64_t clave[kRanuras] = {};
  uint64_t fich[kFicheros] = {};
  uint64_t fin[kFicheros] = {};
  // Totals for the whole session; the summary reports the difference from the previous snapshot.
  uint64_t relecturas = 0, bytes_relectura = 0;
  uint64_t distintos = 0, bytes_distintos = 0;
  uint64_t seguidas = 0, desalojos = 0;
};
EstadoRangos g_rangos;

inline uint64_t HashRuta(const std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ull;
  }
  return h;
}

// Records the range read. Returns true if that exact range had already been read before.
bool AnotarRango(const std::string_view ruta, uint64_t desplazamiento, uint32_t bytes) {
  if (!bytes) {
    return false;
  }
  const uint64_t hr = HashRuta(ruta);
  uint64_t clave = hr ^ (desplazamiento * 0x9E3779B97F4A7C15ull) ^ (uint64_t(bytes) << 1);
  if (clave == 0) {
    clave = 1;  // 0 marks a free slot
  }

  std::lock_guard<std::mutex> lock(g_rangos.mutex);

  // Sequential read: it starts exactly where the previous read of this same file ended.
  const size_t fi = static_cast<size_t>(hr % EstadoRangos::kFicheros);
  if (g_rangos.fich[fi] == hr && g_rangos.fin[fi] == desplazamiento) {
    ++g_rangos.seguidas;
  }
  g_rangos.fich[fi] = hr;
  g_rangos.fin[fi] = desplazamiento + bytes;

  // Repeated range. Open addressing with a few probes: if it does not fit, a slot is overwritten
  // and that is counted.
  const size_t base = static_cast<size_t>((clave >> 17) & (EstadoRangos::kRanuras - 1));
  for (size_t p = 0; p < EstadoRangos::kSondeos; ++p) {
    const size_t j = (base + p) & (EstadoRangos::kRanuras - 1);
    if (g_rangos.clave[j] == clave) {
      ++g_rangos.relecturas;
      g_rangos.bytes_relectura += bytes;
      return true;
    }
    if (g_rangos.clave[j] == 0) {
      g_rangos.clave[j] = clave;
      ++g_rangos.distintos;
      g_rangos.bytes_distintos += bytes;
      return false;
    }
  }
  g_rangos.clave[base] = clave;
  ++g_rangos.distintos;
  g_rangos.bytes_distintos += bytes;
  ++g_rangos.desalojos;  // if this grows, the table is too small and the figures fall short too
  return false;
}

// Snapshot of the previous summary, to report the interval and not the whole-session total.
struct FotoIo {
  uint64_t aperturas_ok = 0, aperturas_fallo = 0, us_aperturas = 0;
  uint64_t lecturas = 0, bytes_leidos = 0, us_lecturas = 0;
  uint64_t misses_en_seco = 0, stats_en_sd = 0, barridos_en_sd = 0;
  uint64_t aciertos = 0, rellenos = 0, directas = 0, bytes_ram = 0;
  uint64_t relecturas = 0, bytes_relectura = 0, distintos = 0, bytes_distintos = 0, seguidas = 0;
};
FotoIo g_io_foto;
std::mutex g_io_foto_mutex;

inline uint64_t AhoraUs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

inline void MaxAtomico(std::atomic<uint64_t>& destino, uint64_t valor) {
  uint64_t visto = destino.load(std::memory_order_relaxed);
  while (valor > visto &&
         !destino.compare_exchange_weak(visto, valor, std::memory_order_relaxed)) {
  }
}

// A single thread writes the summary: whichever wins the exchange of the next deadline.
void QuizaResumenIo(uint64_t ahora_us) {
  const int32_t periodo_s = REXCVAR_GET(nfsmw_io_resumen_s);
  if (periodo_s <= 0) {
    return;
  }
  uint64_t proximo = g_io_proximo_resumen.load(std::memory_order_relaxed);
  const uint64_t paso = static_cast<uint64_t>(periodo_s) * 1000000ull;
  if (proximo == 0) {
    // First call: only the timer is armed, nothing is written yet.
    g_io_proximo_resumen.compare_exchange_strong(proximo, ahora_us + paso,
                                                 std::memory_order_relaxed);
    return;
  }
  if (ahora_us < proximo) {
    return;
  }
  if (!g_io_proximo_resumen.compare_exchange_strong(proximo, ahora_us + paso,
                                                    std::memory_order_relaxed)) {
    return;
  }

  const auto rutas = rex::filesystem::LeerEstadisticasRutas();
  const auto ventana = rex::filesystem::LeerEstadisticasVentana();

  FotoIo ahora;
  ahora.aperturas_ok = g_io.aperturas_ok.load(std::memory_order_relaxed);
  ahora.aperturas_fallo = g_io.aperturas_fallo.load(std::memory_order_relaxed);
  ahora.us_aperturas = g_io.us_aperturas.load(std::memory_order_relaxed);
  ahora.lecturas = g_io.lecturas.load(std::memory_order_relaxed);
  ahora.bytes_leidos = g_io.bytes_leidos.load(std::memory_order_relaxed);
  ahora.us_lecturas = g_io.us_lecturas.load(std::memory_order_relaxed);
  ahora.misses_en_seco = rutas.misses_en_seco;
  ahora.stats_en_sd = rutas.stats_en_sd;
  ahora.barridos_en_sd = rutas.barridos_en_sd;
  ahora.aciertos = ventana.aciertos;
  ahora.rellenos = ventana.rellenos;
  ahora.directas = ventana.directas;
  ahora.bytes_ram = ventana.bytes_ram;
  uint64_t desalojos = 0;
  {
    std::lock_guard<std::mutex> lock(g_rangos.mutex);
    ahora.relecturas = g_rangos.relecturas;
    ahora.bytes_relectura = g_rangos.bytes_relectura;
    ahora.distintos = g_rangos.distintos;
    ahora.bytes_distintos = g_rangos.bytes_distintos;
    ahora.seguidas = g_rangos.seguidas;
    desalojos = g_rangos.desalojos;
  }

  FotoIo d;
  {
    std::lock_guard<std::mutex> lock(g_io_foto_mutex);
    d.aperturas_ok = ahora.aperturas_ok - g_io_foto.aperturas_ok;
    d.aperturas_fallo = ahora.aperturas_fallo - g_io_foto.aperturas_fallo;
    d.us_aperturas = ahora.us_aperturas - g_io_foto.us_aperturas;
    d.lecturas = ahora.lecturas - g_io_foto.lecturas;
    d.bytes_leidos = ahora.bytes_leidos - g_io_foto.bytes_leidos;
    d.us_lecturas = ahora.us_lecturas - g_io_foto.us_lecturas;
    d.misses_en_seco = ahora.misses_en_seco - g_io_foto.misses_en_seco;
    d.stats_en_sd = ahora.stats_en_sd - g_io_foto.stats_en_sd;
    d.barridos_en_sd = ahora.barridos_en_sd - g_io_foto.barridos_en_sd;
    d.aciertos = ahora.aciertos - g_io_foto.aciertos;
    d.rellenos = ahora.rellenos - g_io_foto.rellenos;
    d.directas = ahora.directas - g_io_foto.directas;
    d.bytes_ram = ahora.bytes_ram - g_io_foto.bytes_ram;
    d.relecturas = ahora.relecturas - g_io_foto.relecturas;
    d.bytes_relectura = ahora.bytes_relectura - g_io_foto.bytes_relectura;
    d.distintos = ahora.distintos - g_io_foto.distintos;
    d.bytes_distintos = ahora.bytes_distintos - g_io_foto.bytes_distintos;
    d.seguidas = ahora.seguidas - g_io_foto.seguidas;
    g_io_foto = ahora;
  }

  const uint64_t peor_abrir = g_io.us_apertura_peor.exchange(0, std::memory_order_relaxed);
  const uint64_t peor_leer = g_io.us_lectura_peor.exchange(0, std::memory_order_relaxed);
  const uint64_t aperturas = d.aperturas_ok + d.aperturas_fallo;

  REXKRNL_INFO(
      "[io] {} s: {} aperturas ({} fallan) {:.1f} ms en total, peor {:.1f} ms, media {:.2f} ms; "
      "{} lecturas {:.1f} MB en {:.1f} ms, peor {:.1f} ms",
      periodo_s, aperturas, d.aperturas_fallo, d.us_aperturas / 1000.0, peor_abrir / 1000.0,
      aperturas ? (d.us_aperturas / 1000.0) / double(aperturas) : 0.0, d.lecturas,
      d.bytes_leidos / (1024.0 * 1024.0), d.us_lecturas / 1000.0, peor_leer / 1000.0);
  REXKRNL_INFO(
      "[io] rutas: {} resueltas sin tocar la SD, {} stats, {} barridos de directorio ({} entradas "
      "en el arbol); ventana: {} aciertos / {} rellenos / {} directas, {:.1f} MB desde RAM, {} "
      "ficheros con ventana",
      d.misses_en_seco, d.stats_en_sd, d.barridos_en_sd, rutas.entradas_en_arbol, d.aciertos,
      d.rellenos, d.directas, d.bytes_ram / (1024.0 * 1024.0), ventana.ventanas_vivas);

  /*
   * The line that tells whether a block cache is worth it and whether the window has anything to do.
   *  - "releidas"  = reads whose exact range (file+offset+size) had already been read before.
   *                  A cache would remove those bytes entirely; not the rest.
   *  - "seguidas"  = reads that start exactly where the previous one of the same file ended. If
   *                  this is high, read-ahead helps; if it is low, the game jumps around and the
   *                  window only gets in the way.
   *  - "desalojos" = times the range table ran out of room. If it grows, the figures fall short.
   */
  const uint64_t lecturas_tramo = d.relecturas + d.distintos;
  REXKRNL_INFO(
      "[io] rangos: {} releidas de {} ({:.0f} %), {:.1f} MB ya leidos antes de {:.1f} MB ({:.0f} %); {} "
      "seguidas ({:.0f} %); {} desalojos",
      d.relecturas, lecturas_tramo,
      lecturas_tramo ? 100.0 * double(d.relecturas) / double(lecturas_tramo) : 0.0,
      d.bytes_relectura / (1024.0 * 1024.0),
      (d.bytes_relectura + d.bytes_distintos) / (1024.0 * 1024.0),
      (d.bytes_relectura + d.bytes_distintos)
          ? 100.0 * double(d.bytes_relectura) / double(d.bytes_relectura + d.bytes_distintos)
          : 0.0,
      d.seguidas, lecturas_tramo ? 100.0 * double(d.seguidas) / double(lecturas_tramo) : 0.0,
      desalojos);

  /*
   * The cache of large reads by exact range (nfsmw_io_rangos_mb).
   *
   * It is accumulated since startup, not per interval: there are few events and what matters is
   * the session total. And it is always printed, whether it is on or not and whether there was
   * activity or not: there was already a counter that existed and was never printed, and without
   * the line there is no way to tell whether it works.
   *
   * How to read it:
   *  - "expulsiones" at 0 is what is expected. If it grows, the cap is too small and the LRU
   *    drops entries before reusing them, which is exactly how the block cache died.
   *  - "de la SD" against "desde RAM" is the amplification. By exact range it must be 1:1 on
   *    the first lap and go down from there; if more SD were read than delivered, something is
   *    wrong (the block cache read 318 for 304 delivered, and that is why it was turned off).
   */
  const auto rangos = rex::filesystem::LeerEstadisticasRangos();
  const uint64_t rangos_total = rangos.aciertos + rangos.fallos;
  REXKRNL_INFO(
      "[io] rangos-cache: {} aciertos / {} fallos ({:.0f} %), {:.1f} MB desde RAM contra {:.1f} MB "
      "de la SD; {} entradas vivas ({:.1f} MB de {} tope), {} expulsiones; no cacheadas: {} bajo el "
      "suelo de {} KB, {} sobre el techo, {} del barrido de carga ({:.1f} MB){}",
      rangos.aciertos, rangos.fallos,
      rangos_total ? 100.0 * double(rangos.aciertos) / double(rangos_total) : 0.0,
      rangos.bytes_ram / (1024.0 * 1024.0), rangos.bytes_disco / (1024.0 * 1024.0), rangos.entradas,
      rangos.bytes_vivos / (1024.0 * 1024.0), rangos.tope_mb, rangos.expulsiones, rangos.bajo_suelo,
      rangos.suelo_kb, rangos.sobre_techo, rangos.secuenciales,
      rangos.secuenciales_bytes / (1024.0 * 1024.0), rangos.sin_memoria ? " [APAGADA: sin memoria]" : "");
}

// Always measured: reading the monotonic clock on Horizon is a processor register read, not a system call.
inline void AnotarApertura(uint64_t us, bool ok, const std::string_view ruta) {
  if (ok) {
    g_io.aperturas_ok.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_io.aperturas_fallo.fetch_add(1, std::memory_order_relaxed);
  }
  g_io.us_aperturas.fetch_add(us, std::memory_order_relaxed);
  MaxAtomico(g_io.us_apertura_peor, us);

  const int32_t aviso_ms = REXCVAR_GET(nfsmw_io_aviso_ms);
  if (aviso_ms > 0 && us >= static_cast<uint64_t>(aviso_ms) * 1000ull &&
      g_io_avisos.fetch_add(1, std::memory_order_relaxed) < 300) {
    REXKRNL_WARN("[io] LENTO: abrir '{}' tardo {:.1f} ms ({})", ruta, us / 1000.0,
                 ok ? "abierto" : "no existe");
  }
}

inline void AnotarLectura(uint64_t us, uint32_t bytes, const std::string_view ruta,
                          uint64_t desplazamiento) {
  g_io.lecturas.fetch_add(1, std::memory_order_relaxed);
  g_io.bytes_leidos.fetch_add(bytes, std::memory_order_relaxed);
  g_io.us_lecturas.fetch_add(us, std::memory_order_relaxed);
  MaxAtomico(g_io.us_lectura_peor, us);

  const bool releida = AnotarRango(ruta, desplazamiento, bytes);

  const int32_t aviso_ms = REXCVAR_GET(nfsmw_io_aviso_ms);
  if (aviso_ms > 0 && us >= static_cast<uint64_t>(aviso_ms) * 1000ull &&
      g_io_avisos.fetch_add(1, std::memory_order_relaxed) < 300) {
    // The offset is included. Without it a sequential sweep cannot be told apart from a reread.
    REXKRNL_WARN("[io] LENTO: leer {} bytes de '{}' en {} tardo {:.1f} ms{}", bytes, ruta,
                 desplazamiento, us / 1000.0, releida ? " (RELEIDA)" : "");
  }
}

}  // namespace

// NFSMW: last .wmv movie opened by the game. The app reopens it through the VFS and decodes it
// natively with FFmpeg, because the recompiled XDK WMV decoder does not reach real time on the
// Switch.
namespace {
std::mutex g_nfsmw_wmv_mutex;
std::string g_nfsmw_ultimo_wmv;
std::vector<std::pair<const rex::filesystem::Entry*, std::string>> g_nfsmw_wmv_abiertos;
std::string g_nfsmw_ultimo_wmv_leido;

void NfsmwAnotarApertura(const rex::filesystem::Entry* entrada, const std::string& ruta) {
  std::lock_guard<std::mutex> lock(g_nfsmw_wmv_mutex);
  g_nfsmw_ultimo_wmv = ruta;
  for (auto& [e, r] : g_nfsmw_wmv_abiertos) {
    if (e == entrada) {
      r = ruta;
      return;
    }
  }
  if (g_nfsmw_wmv_abiertos.size() >= 64) {
    g_nfsmw_wmv_abiertos.erase(g_nfsmw_wmv_abiertos.begin());
  }
  g_nfsmw_wmv_abiertos.emplace_back(entrada, ruta);
  REXLOG_INFO("[video] el juego abre '{}'", ruta);
}

void NfsmwAnotarLectura(const rex::filesystem::Entry* entrada) {
  std::lock_guard<std::mutex> lock(g_nfsmw_wmv_mutex);
  for (const auto& [e, r] : g_nfsmw_wmv_abiertos) {
    if (e == entrada) {
      if (r != g_nfsmw_ultimo_wmv_leido) {
        g_nfsmw_ultimo_wmv_leido = r;
        REXLOG_INFO("[video] el juego lee '{}'", r);
      }
      return;
    }
  }
}
}  // namespace

std::string NfsmwUltimoWmvAbierto() {
  std::lock_guard<std::mutex> lock(g_nfsmw_wmv_mutex);
  return g_nfsmw_ultimo_wmv;
}

// NFSMW: last .wmv movie read by the game: the one being played.
std::string NfsmwUltimoWmvLeido() {
  std::lock_guard<std::mutex> lock(g_nfsmw_wmv_mutex);
  return g_nfsmw_ultimo_wmv_leido;
}

struct CreateOptions {
  // https://processhacker.sourceforge.io/doc/ntioapi_8h.html
  static const uint32_t FILE_DIRECTORY_FILE = 0x00000001;
  // Optimization - files access will be sequential, not random.
  static const uint32_t FILE_SEQUENTIAL_ONLY = 0x00000004;
  static const uint32_t FILE_SYNCHRONOUS_IO_ALERT = 0x00000010;
  static const uint32_t FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020;
  static const uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;
  // Optimization - file access will be random, not sequential.
  static const uint32_t FILE_RANDOM_ACCESS = 0x00000800;
};

static bool IsValidPath(const std::string_view s, bool is_pattern) {
  // TODO(gibbed): validate path components individually
  bool got_asterisk = false;
  for (const auto& c : s) {
    if (c <= 31 || c >= 127) {
      return false;
    }
    if (got_asterisk) {
      // * must be followed by a . (*.)
      //
      // 4D530819 has a bug in its game code where it attempts to
      // FindFirstFile() with filters of "Game:\\*_X3.rkv", "Game:\\m*_X3.rkv",
      // and "Game:\\w*_X3.rkv" and will infinite loop if the path filter is
      // allowed.
      if (c != '.') {
        return false;
      }
      got_asterisk = false;
    }
    switch (c) {
      case '"':
      // case '*':
      case '+':
      case ',':
      // case ':':
      // case ';':
      case '<':
      // case '=':
      case '>':
      // case '?':
      case '|': {
        return false;
      }
      case '*': {
        // Pattern-specific (for NtQueryDirectoryFile)
        if (!is_pattern) {
          return false;
        }
        got_asterisk = true;
        break;
      }
      case '?': {
        // Pattern-specific (for NtQueryDirectoryFile)
        if (!is_pattern) {
          return false;
        }
        break;
      }
      default: {
        break;
      }
    }
  }
  return true;
}

u32 NtCreateFile_entry(mapped_u32 handle_out, u32 desired_access,
                       ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attrs,
                       ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_u64 allocation_size_ptr,
                       u32 file_attributes, u32 share_access, u32 creation_disposition,
                       u32 create_options) {
  // note used. maybe later
  // uint64_t allocation_size = 0;  // is this correct???
  // if (allocation_size_ptr) {
  //  allocation_size = *allocation_size_ptr;
  //}

  if (!object_attrs) {
    // ..? Some games do this. This parameter is not optional.
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);

  rex::filesystem::Entry* root_entry = nullptr;

  // Compute path, possibly attrs relative.
  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
  REXKRNL_IMPORT_TRACE(
      "NtCreateFile", "path={} access={:#x} attrs={:#x} share={:#x} disp={:#x} options={:#x}",
      target_path, (uint32_t)desired_access, (uint32_t)file_attributes, (uint32_t)share_access,
      (uint32_t)creation_disposition, (uint32_t)create_options);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      object_attrs->root_directory != 0) {
    auto root_file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(object_attrs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);

    root_entry = root_file->entry();
  }

  // Attempt open (or create).
  rex::filesystem::File* vfs_file;
  rex::filesystem::FileAction file_action;
  const uint64_t io_inicio_us = AhoraUs();
  X_STATUS result = REX_KERNEL_FS()->OpenFile(
      root_entry, target_path, rex::filesystem::FileDisposition((uint32_t)creation_disposition),
      desired_access, (create_options & CreateOptions::FILE_DIRECTORY_FILE) != 0,
      (create_options & CreateOptions::FILE_NON_DIRECTORY_FILE) != 0, &vfs_file, &file_action);
  const uint64_t io_fin_us = AhoraUs();
  AnotarApertura(io_fin_us - io_inicio_us, XSUCCEEDED(result), target_path);
  QuizaResumenIo(io_fin_us);
  object_ref<XFile> file = nullptr;

  X_HANDLE handle = X_INVALID_HANDLE_VALUE;
  if (XSUCCEEDED(result)) {
    // If true, desired_access SYNCHRONIZE flag must be set.
    bool synchronous = (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_ALERT) ||
                       (create_options & CreateOptions::FILE_SYNCHRONOUS_IO_NONALERT);
    file = object_ref<XFile>(new XFile(REX_KERNEL_STATE(), vfs_file, synchronous));

    // Handle ref is incremented, so return that.
    handle = file->handle();

    // NFSMW: records the .wmv movies (see NfsmwUltimoWmvAbierto).
    if (target_path.size() > 4) {
      std::string extension(target_path.substr(target_path.size() - 4));
      for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      }
      if (extension == ".wmv") {
        NfsmwAnotarApertura(vfs_file->entry(), std::string(target_path));
      }
    }
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = (uint32_t)file_action;
  }

  *handle_out = handle;
  if (XFAILED(result)) {
    REXKRNL_IMPORT_FAIL("NtCreateFile", "path='{}' -> {:#x}", target_path, result);
  } else {
    REXKRNL_IMPORT_RESULT("NtCreateFile", "{:#x} handle={:#x}", result, handle);
  }
  return result;
}

u32 NtOpenFile_entry(mapped_u32 handle_out, u32 desired_access,
                     ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attributes,
                     ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, u32 open_options) {
  return NtCreateFile_entry(handle_out, desired_access, object_attributes, io_status_block, nullptr,
                            0, 0, static_cast<uint32_t>(rex::filesystem::FileDisposition::kOpen),
                            open_options);
}

u32 NtReadFile_entry(u32 file_handle, u32 event_handle, mapped_void apc_routine_ptr,
                     mapped_void apc_context, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                     mapped_void buffer, u32 buffer_length, mapped_u64 byte_offset_ptr) {
  uint64_t byte_offset = byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : 0;
  const bool apc_requested = (static_cast<uint32_t>(apc_routine_ptr) & ~1u) != 0;
  REXKRNL_IMPORT_TRACE(
      "NtReadFile",
      "handle={:#x} event={:#x} apc={:#x} apc_ctx={:#x} iosb={:#x} buf={:#x} len={:#x} offset={}",
      (uint32_t)file_handle, (uint32_t)event_handle, apc_routine_ptr.guest_address(),
      apc_context.guest_address(), io_status_block.guest_address(), buffer.guest_address(),
      (uint32_t)buffer_length, byte_offset_ptr ? (int64_t)byte_offset : -1);
  X_STATUS result = X_STATUS_SUCCESS;
  bool apc_queued = false;

  // NFSMW: last .wmv movie read (see NfsmwUltimoWmvLeido).
  if (auto leido = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle)) {
    NfsmwAnotarLectura(leido->entry());
  }

  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    /*
     * Why the "true ||" stays, and what happens if it is removed.
     *
     * It was not added by this port: it comes unchanged from Xenia and upstream ReXGlue, in
     * NtReadFile, NtReadFileScatter and NtWriteFile. The else branch below is Xenia's unfinished gap
     * ("TODO(benvanik): async."): it reads nothing, it only writes PENDING to the status block and
     * returns PENDING. Removing the "true ||" does not make the I/O asynchronous, it makes every
     * file opened without FILE_SYNCHRONOUS_IO return zero bytes.
     *
     * And the game opens files that way. In the recompiled code NtReadFile is called from three places:
     *   0x8284FE78  event=0, apc=0, ctx=0, 64 KB per iteration in a loop. Right after, it reads
     *               information from the status block without waiting for anything. It expects an
     *               already completed answer.
     *   0x8284DB54  event=0, apc=0, ctx=0. Compares the result with 259 (0x103 = PENDING) and has a
     *               path for that case.
     *   0x8284DAF4  event = [r31+16], apc=0, ctx, status block = r31, and before the call it writes
     *               259 to r31+0. That one is a real asynchronous path, OVERLAPPED style.
     *
     * Honoring the asynchrony (an I/O thread that, on completion, writes the status block, signals
     * the event and queues the APC) was evaluated and rejected, because measured it gains nothing:
     * reads already run on a thread other than the render thread, and outside loading screens there
     * is no correlation between slow reads and stutters (1 of 17 stutters over 150 ms falls within
     * 0.25 s of a slow read, against a base rate of 5 %). The only place it could gain is inside
     * loading, if the game overlaps; that is 29.7 s out of 483. Not worth the risk.
     */
    if (true || file->is_synchronous()) {
      // Synchronous.
      uint32_t bytes_read = 0;
      // Where the read starts. If no offset is given, the game reads from the file position.
      const uint64_t desplazamiento =
          byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : file->position();
      const uint64_t io_inicio_us = AhoraUs();
      result = file->Read(buffer.guest_address(), buffer_length,
                          byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
                          &bytes_read, apc_context.guest_address());
      const uint64_t io_fin_us = AhoraUs();
      AnotarLectura(io_fin_us - io_inicio_us, bytes_read, file->path(), desplazamiento);
      QuizaResumenIo(io_fin_us);
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine_ptr & ~1) {
        if (apc_context && result == X_STATUS_SUCCESS) {
          auto thread = XThread::GetCurrentThread();
          uint32_t apc_routine = static_cast<uint32_t>(apc_routine_ptr) & ~1u;
          uint32_t apc_ctx = apc_context.guest_address();
          uint32_t apc_arg1 = io_status_block.guest_address();
          REXKRNL_IMPORT_TRACE("NtReadFile",
                               "queue_apc thid={} normal={:#x} ctx={:#x} arg1={:#x} arg2=0",
                               thread ? thread->thread_id() : 0, apc_routine, apc_ctx, apc_arg1);
          thread->EnqueueApc(apc_routine, apc_ctx, apc_arg1, 0);
          apc_queued = true;
        } else {
          REXKRNL_IMPORT_TRACE("NtReadFile", "skip_apc_queue (apc_ctx={:#x}, status={:#x})",
                               apc_context.guest_address(), result);
        }
      }

      if (!file->is_synchronous() && result != X_STATUS_END_OF_FILE) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // TODO(benvanik): async.

      // X_STATUS_PENDING if not returning immediately.
      // XFile is waitable and signalled after each async req completes.
      // reset the input event (->Reset())
      /*xeNtReadFileState* call_state = new xeNtReadFileState();
      XAsyncRequest* request = new XAsyncRequest(
      state, file,
      (XAsyncRequest::CompletionCallback)xeNtReadFileCompleted,
      call_state);*/
      // result = file->Read(buffer.guest_address(), buffer_length, byte_offset,
      //                     request);
      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }

      result = X_STATUS_PENDING;
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  // Log detailed completion info for debugging async IO issues
  if (file) {
    REXKRNL_IMPORT_RESULT(
        "NtReadFile",
        "{:#x} (sync={}, iosb_status={:#x}, iosb_info={}, ev_signaled={}, apc_requested={}, "
        "apc_queued={})",
        result, file->is_synchronous(),
        io_status_block ? (uint32_t)io_status_block->status : 0xDEAD,
        io_status_block ? (uint32_t)io_status_block->information : 0, ev && signal_event,
        apc_requested, apc_queued);
  } else {
    REXKRNL_IMPORT_RESULT("NtReadFile", "{:#x} (apc_requested={}, apc_queued={})", result,
                          apc_requested, apc_queued);
  }
  return result;
}

u32 NtReadFileScatter_entry(u32 file_handle, u32 event_handle, mapped_void apc_routine_ptr,
                            mapped_void apc_context, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                            mapped_u32 segment_array, u32 length, mapped_u64 byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  if (XSUCCEEDED(result)) {
    if (true || file->is_synchronous()) {
      // Synchronous.
      uint32_t bytes_read = 0;
      result = file->ReadScatter(segment_array.guest_address(), length,
                                 byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
                                 &bytes_read, apc_context.guest_address());
      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = bytes_read;
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine_ptr & ~1) {
        if (apc_context) {
          auto thread = XThread::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine_ptr) & ~1u,
                             apc_context.guest_address(), io_status_block.guest_address(), 0);
        }
      }

      if (!file->is_synchronous()) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // TODO(benvanik): async.

      // TODO: On Windows it might be worth trying to use Win32 ReadFileScatter
      // here instead of handling it ourselves

      // X_STATUS_PENDING if not returning immediately.
      // XFile is waitable and signalled after each async req completes.
      // reset the input event (->Reset())
      /*xeNtReadFileState* call_state = new xeNtReadFileState();
      XAsyncRequest* request = new XAsyncRequest(
      state, file,
      (XAsyncRequest::CompletionCallback)xeNtReadFileCompleted,
      call_state);*/
      // result = file->Read(buffer.guest_address(), buffer_length, byte_offset,
      //                     request);
      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }

      result = X_STATUS_PENDING;
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  return result;
}

u32 NtWriteFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine, mapped_void apc_context,
                      ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_void buffer,
                      u32 buffer_length, mapped_u64 byte_offset_ptr) {
  X_STATUS result = X_STATUS_SUCCESS;

  // Grab event to signal.
  bool signal_event = false;
  auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
  if (event_handle && !ev) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Grab file.
  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  }

  // Execute write.
  if (XSUCCEEDED(result)) {
    // TODO(benvanik): async path.
    if (true || file->is_synchronous()) {
      // Synchronous request.
      uint32_t bytes_written = 0;
      result = file->Write(buffer.guest_address(), buffer_length,
                           byte_offset_ptr ? static_cast<uint64_t>(*byte_offset_ptr) : -1,
                           &bytes_written, apc_context.guest_address());

      if (io_status_block) {
        io_status_block->status = result;
        io_status_block->information = static_cast<uint32_t>(bytes_written);
      }

      // Queue the APC callback. It must be delivered via the APC mechanism even
      // though were are completing immediately.
      // Low bit probably means do not queue to IO ports.
      if ((uint32_t)apc_routine & ~1) {
        if (apc_context) {
          auto thread = XThread::GetCurrentThread();
          thread->EnqueueApc(static_cast<uint32_t>(apc_routine) & ~1u, apc_context.guest_address(),
                             io_status_block.guest_address(), 0);
        }
      }

      if (!file->is_synchronous()) {
        result = X_STATUS_PENDING;
      }

      // Mark that we should signal the event now. We do this after
      // we have written the info out.
      signal_event = true;
    } else {
      // X_STATUS_PENDING if not returning immediately.
      result = X_STATUS_PENDING;

      if (io_status_block) {
        io_status_block->status = X_STATUS_PENDING;
        io_status_block->information = 0;
      }
    }
  }

  if (XFAILED(result) && io_status_block) {
    io_status_block->status = result;
    io_status_block->information = 0;
  }

  if (ev && signal_event) {
    ev->Set(0, false);
  }

  return result;
}

u32 NtCreateIoCompletion_entry(mapped_u32 out_handle, u32 desired_access,
                               mapped_void object_attribs, u32 num_concurrent_threads) {
  auto completion = new XIOCompletion(REX_KERNEL_STATE());
  if (out_handle) {
    *out_handle = completion->handle();
  }

  return X_STATUS_SUCCESS;
}

u32 NtSetIoCompletion_entry(u32 handle, u32 key_context, u32 apc_context, u32 completion_status,
                            u32 num_bytes) {
  auto port = REX_KERNEL_OBJECTS()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    return X_STATUS_INVALID_HANDLE;
  }

  XIOCompletion::IONotification notification;
  notification.key_context = key_context;
  notification.apc_context = apc_context;
  notification.num_bytes = num_bytes;
  notification.status = completion_status;

  port->QueueNotification(notification);
  return X_STATUS_SUCCESS;
}

// Dequeues a packet from the completion port.
u32 NtRemoveIoCompletion_entry(u32 handle, mapped_u32 key_context, mapped_u32 apc_context,
                               ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block, mapped_u64 timeout) {
  X_STATUS status = X_STATUS_SUCCESS;
  // uint32_t info = 0;

  auto port = REX_KERNEL_OBJECTS()->LookupObject<XIOCompletion>(handle);
  if (!port) {
    status = X_STATUS_INVALID_HANDLE;
  }

  uint64_t timeout_ticks = timeout ? static_cast<uint32_t>(*timeout)
                                   : static_cast<uint64_t>(std::numeric_limits<int64_t>::min());
  XIOCompletion::IONotification notification;
  if (port->WaitForNotification(timeout_ticks, &notification)) {
    if (key_context) {
      *key_context = notification.key_context;
    }
    if (apc_context) {
      *apc_context = notification.apc_context;
    }

    if (io_status_block) {
      io_status_block->status = notification.status;
      io_status_block->information = notification.num_bytes;
    }
  } else {
    status = X_STATUS_TIMEOUT;
  }

  return status;
}

u32 NtQueryFullAttributesFile_entry(ppc_ptr_t<X_OBJECT_ATTRIBUTES> obj_attribs,
                                    ppc_ptr_t<X_FILE_NETWORK_OPEN_INFORMATION> file_info) {
  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(obj_attribs->name_ptr);
  auto path_str = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);
  REXKRNL_IMPORT_TRACE("NtQueryFullAttributesFile", "path={}", path_str);

  object_ref<XFile> root_file;
  if (obj_attribs->root_directory != 0xFFFFFFFD &&  // ObDosDevices
      obj_attribs->root_directory != 0) {
    root_file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(obj_attribs->root_directory);
    assert_not_null(root_file);
    assert_true(root_file->type() == XObject::Type::File);
    assert_always();
  }

  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  // Resolve the file using the virtual file system.
  auto entry = REX_KERNEL_FS()->ResolvePath(target_path);
  if (entry) {
    // Found.
    file_info->creation_time = entry->create_timestamp();
    file_info->last_access_time = entry->access_timestamp();
    file_info->last_write_time = entry->write_timestamp();
    file_info->change_time = entry->write_timestamp();
    file_info->allocation_size = entry->allocation_size();
    file_info->end_of_file = entry->size();
    file_info->attributes = entry->attributes();

    REXKRNL_IMPORT_RESULT("NtQueryFullAttributesFile", "0x0 size={}", (uint64_t)entry->size());
    return X_STATUS_SUCCESS;
  }

  REXKRNL_IMPORT_RESULT("NtQueryFullAttributesFile", "X_STATUS_NO_SUCH_FILE");
  return X_STATUS_NO_SUCH_FILE;
}

u32 NtQueryDirectoryFile_entry(u32 file_handle, u32 event_handle, u32 apc_routine,
                               mapped_void apc_context,
                               ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block,
                               ppc_ptr_t<X_FILE_DIRECTORY_INFORMATION> file_info_ptr, u32 length,
                               ppc_ptr_t<X_ANSI_STRING> file_name, u32 restart_scan) {
  if (length < 72) {
    return X_STATUS_INFO_LENGTH_MISMATCH;
  }

  X_STATUS result = X_STATUS_UNSUCCESSFUL;
  uint32_t info = 0;

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  auto name = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), file_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(name, true)) {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (file) {
    // X_FILE_DIRECTORY_INFORMATION dir_info = {0};
    result = file->QueryDirectory(file_info_ptr, length, name, restart_scan != 0);
    if (XSUCCEEDED(result)) {
      info = length;
    }
  } else {
    result = X_STATUS_NO_SUCH_FILE;
  }

  if (XFAILED(result)) {
    info = 0;
  }

  if (io_status_block) {
    io_status_block->status = result;
    io_status_block->information = info;
  }

  return result;
}

/*
 * This used to lie: it returned SUCCESS without even looking at the handle.
 *
 * The game calls it right after saving: it is its way of saying "this must be on disk now". If
 * nobody honors it, what was written is at the mercy of whenever the file system decides to flush
 * it, and on the Switch that means that leaving through the HOME menu or turning the console off
 * right after saving can leave the save half written on the SD. It shows up as the game's
 * damaged-profile message ("Parece que <perfil> esta danado y no puede utilizarse" in the Spanish
 * edition).
 */
u32 NtFlushBuffersFile_entry(u32 file_handle, ppc_ptr_t<X_IO_STATUS_BLOCK> io_status_block_ptr) {
  auto result = X_STATUS_SUCCESS;

  auto file = REX_KERNEL_OBJECTS()->LookupObject<XFile>(file_handle);
  if (!file) {
    result = X_STATUS_INVALID_HANDLE;
  } else {
    result = file->Flush();
    static std::atomic<uint32_t> avisos{0};
    if (avisos.fetch_add(1, std::memory_order_relaxed) < 16) {
      REXKRNL_INFO("[guardado] NtFlushBuffersFile: {} bajado al disco (resultado {:08X})",
                   file->path(), uint32_t(result));
    }
  }

  if (io_status_block_ptr) {
    io_status_block_ptr->status = result;
    io_status_block_ptr->information = 0;
  }

  return result;
}

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntopensymboliclinkobject
u32 NtOpenSymbolicLinkObject_entry(mapped_u32 handle_out,
                                   ppc_ptr_t<X_OBJECT_ATTRIBUTES> object_attrs) {
  if (!object_attrs) {
    return X_STATUS_INVALID_PARAMETER;
  }
  assert_not_null(handle_out);

  assert_true(object_attrs->attributes == 64);  // case insensitive

  auto object_name = REX_KERNEL_MEMORY()->TranslateVirtual<X_ANSI_STRING*>(object_attrs->name_ptr);

  auto target_path = util::TranslateAnsiPath(REX_KERNEL_MEMORY(), object_name);

  // Enforce that the path is ASCII.
  if (!IsValidPath(target_path, false)) {
    return X_STATUS_OBJECT_NAME_INVALID;
  }

  if (object_attrs->root_directory != 0) {
    assert_always();
  }

  if (rex::string::utf8_starts_with(target_path, "\\??\\")) {
    target_path = target_path.substr(4);  // Strip the full qualifier
  }

  std::string link_path;
  if (!REX_KERNEL_FS()->FindSymbolicLink(target_path, link_path)) {
    return X_STATUS_NO_SUCH_FILE;
  }

  object_ref<XSymbolicLink> symlink(new XSymbolicLink(REX_KERNEL_STATE()));
  symlink->Initialize(target_path, link_path);

  *handle_out = symlink->handle();

  return X_STATUS_SUCCESS;
}

// https://docs.microsoft.com/en-us/windows/win32/devnotes/ntquerysymboliclinkobject
u32 NtQuerySymbolicLinkObject_entry(u32 handle, ppc_ptr_t<X_ANSI_STRING> target) {
  auto symlink = REX_KERNEL_OBJECTS()->LookupObject<XSymbolicLink>(handle);
  if (!symlink) {
    return X_STATUS_NO_SUCH_FILE;
  }
  auto length = std::min(static_cast<size_t>(target->maximum_length), symlink->target().size());
  if (length > 0) {
    auto target_buf = REX_KERNEL_MEMORY()->TranslateVirtual(target->pointer);
    std::memcpy(target_buf, symlink->target().c_str(), length);
  }
  target->length = static_cast<uint16_t>(length);
  return X_STATUS_SUCCESS;
}

u32 FscGetCacheElementCount_entry(u32 r3) {
  return 0;
}

u32 FscSetCacheElementCount_entry(u32 unk_0, u32 unk_1) {
  // unk_0 = 0
  // unk_1 looks like a count? in what units? 256 is a common value
  return X_STATUS_SUCCESS;
}

u32 NtDeviceIoControlFile_entry(u32 handle, u32 event_handle, u32 apc_routine, u32 apc_context,
                                u32 io_status_block, u32 io_control_code, mapped_void input_buffer,
                                u32 input_buffer_len, mapped_void output_buffer,
                                u32 output_buffer_len) {
  // Called by XMountUtilityDrive cache-mounting code
  // (checks if the returned values look valid, values below seem to pass the
  // checks)
  const uint32_t cache_size = 0xFF000;

  const uint32_t X_IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000;
  const uint32_t X_IOCTL_DISK_GET_PARTITION_INFO = 0x74004;

  if (io_control_code == X_IOCTL_DISK_GET_DRIVE_GEOMETRY) {
    if (output_buffer_len < 0x8) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    memory::store_and_swap<uint32_t>(output_buffer, cache_size / 512);
    memory::store_and_swap<uint32_t>(output_buffer + 4, 512);
  } else if (io_control_code == X_IOCTL_DISK_GET_PARTITION_INFO) {
    if (output_buffer_len < 0x10) {
      assert_always();
      return X_STATUS_BUFFER_TOO_SMALL;
    }
    memory::store_and_swap<uint64_t>(output_buffer, 0);
    memory::store_and_swap<uint64_t>(output_buffer + 8, cache_size);
  } else {
    REXKRNL_DEBUG("NtDeviceIoControlFile(0x{:X}) - unhandled IOCTL!", uint32_t(io_control_code));
    assert_always();
    return X_STATUS_INVALID_PARAMETER;
  }

  return X_STATUS_SUCCESS;
}

u32 IoCreateDevice_entry(u32 device_struct, u32 r4, u32 r5, u32 r6, u32 r7, mapped_u32 out_struct) {
  // Called from XMountUtilityDrive XAM-task code
  // That code tries writing things to a pointer at out_struct+0x18
  // We'll alloc some scratch space for it so it doesn't cause any exceptions

  // 0x24 is guessed size from accesses to out_struct - likely incorrect
  auto out_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x24);

  auto out = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(out_guest);
  memset(out, 0, 0x24);

  // XMountUtilityDrive writes some kind of header here
  // 0x1000 bytes should be enough to store it
  auto out_guest2 = REX_KERNEL_MEMORY()->SystemHeapAlloc(0x1000);
  memory::store_and_swap(out + 0x18, out_guest2);

  *out_struct = out_guest;
  return X_STATUS_SUCCESS;
}

u32 IoDismountVolumeByFileHandle_entry(u32 handle) {
  REXKRNL_WARN("IoDismountVolumeByFileHandle({:#x}) - stub", (uint32_t)handle);
  return X_STATUS_SUCCESS;
}

u32 IoDismountVolumeByName_entry(ppc_ptr_t<X_ANSI_STRING> name) {
  REXKRNL_WARN("IoDismountVolumeByName - stub");
  return X_STATUS_SUCCESS;
}

u32 IoSynchronousDeviceIoControlRequest_entry(u32 ioctl, mapped_void device_object,
                                              mapped_void input_buffer, u32 input_length,
                                              mapped_void output_buffer, u32 output_length,
                                              mapped_u32 returned_length, u32 unk) {
  REXKRNL_WARN("IoSynchronousDeviceIoControlRequest({:#x}) - stub", (uint32_t)ioctl);
  return X_STATUS_SUCCESS;
}

u32 StfsCreateDevice_entry(mapped_void device_object, u32 flags, mapped_u32 out_device) {
  REXKRNL_WARN("StfsCreateDevice - stub");
  // if (out_device) *out_device = 0;
  return X_STATUS_SUCCESS;
}

u32 StfsControlDevice_entry(mapped_void device_object, u32 ioctl, mapped_void input_buffer,
                            u32 input_length, mapped_void output_buffer, u32 output_length) {
  REXKRNL_WARN("StfsControlDevice({:#x}) - stub", (uint32_t)ioctl);
  return X_STATUS_SUCCESS;
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__NtCreateFile, rex::kernel::xboxkrnl::NtCreateFile_entry)
REX_EXPORT(__imp__NtOpenFile, rex::kernel::xboxkrnl::NtOpenFile_entry)
REX_EXPORT(__imp__NtReadFile, rex::kernel::xboxkrnl::NtReadFile_entry)
REX_EXPORT(__imp__NtReadFileScatter, rex::kernel::xboxkrnl::NtReadFileScatter_entry)
REX_EXPORT(__imp__NtWriteFile, rex::kernel::xboxkrnl::NtWriteFile_entry)
REX_EXPORT(__imp__NtCreateIoCompletion, rex::kernel::xboxkrnl::NtCreateIoCompletion_entry)
REX_EXPORT(__imp__NtSetIoCompletion, rex::kernel::xboxkrnl::NtSetIoCompletion_entry)
REX_EXPORT(__imp__NtRemoveIoCompletion, rex::kernel::xboxkrnl::NtRemoveIoCompletion_entry)
REX_EXPORT(__imp__NtQueryFullAttributesFile, rex::kernel::xboxkrnl::NtQueryFullAttributesFile_entry)
REX_EXPORT(__imp__NtQueryDirectoryFile, rex::kernel::xboxkrnl::NtQueryDirectoryFile_entry)
REX_EXPORT(__imp__NtFlushBuffersFile, rex::kernel::xboxkrnl::NtFlushBuffersFile_entry)
REX_EXPORT(__imp__NtOpenSymbolicLinkObject, rex::kernel::xboxkrnl::NtOpenSymbolicLinkObject_entry)
REX_EXPORT(__imp__NtQuerySymbolicLinkObject, rex::kernel::xboxkrnl::NtQuerySymbolicLinkObject_entry)
REX_EXPORT(__imp__FscGetCacheElementCount, rex::kernel::xboxkrnl::FscGetCacheElementCount_entry)
REX_EXPORT(__imp__FscSetCacheElementCount, rex::kernel::xboxkrnl::FscSetCacheElementCount_entry)
REX_EXPORT(__imp__NtDeviceIoControlFile, rex::kernel::xboxkrnl::NtDeviceIoControlFile_entry)
REX_EXPORT(__imp__IoCreateDevice, rex::kernel::xboxkrnl::IoCreateDevice_entry)
REX_EXPORT(__imp__IoDismountVolumeByFileHandle,
           rex::kernel::xboxkrnl::IoDismountVolumeByFileHandle_entry)
REX_EXPORT(__imp__IoDismountVolumeByName, rex::kernel::xboxkrnl::IoDismountVolumeByName_entry)
REX_EXPORT(__imp__IoSynchronousDeviceIoControlRequest,
           rex::kernel::xboxkrnl::IoSynchronousDeviceIoControlRequest_entry)
REX_EXPORT(__imp__StfsCreateDevice, rex::kernel::xboxkrnl::StfsCreateDevice_entry)
REX_EXPORT(__imp__StfsControlDevice, rex::kernel::xboxkrnl::StfsControlDevice_entry)

REX_EXPORT_STUB(__imp__IoAcquireDeviceObjectLock);
REX_EXPORT_STUB(__imp__IoAllocateIrp);
REX_EXPORT_STUB(__imp__IoBuildAsynchronousFsdRequest);
REX_EXPORT_STUB(__imp__IoBuildDeviceIoControlRequest);
REX_EXPORT_STUB(__imp__IoBuildSynchronousFsdRequest);
REX_EXPORT_STUB(__imp__IoCallDriver);
REX_EXPORT_STUB(__imp__IoCheckShareAccess);
REX_EXPORT_STUB(__imp__IoCompleteRequest);
REX_EXPORT_STUB(__imp__IoCreateFile);
REX_EXPORT_STUB(__imp__IoDeleteDevice);
REX_EXPORT_STUB(__imp__IoDismountVolume);
REX_EXPORT_STUB(__imp__IoFreeIrp);
REX_EXPORT_STUB(__imp__IoInitializeIrp);
REX_EXPORT_STUB(__imp__IoInvalidDeviceRequest);
REX_EXPORT_STUB(__imp__IoQueueThreadIrp);
REX_EXPORT_STUB(__imp__IoReleaseDeviceObjectLock);
REX_EXPORT_STUB(__imp__IoRemoveShareAccess);
REX_EXPORT_STUB(__imp__IoSetIoCompletion);
REX_EXPORT_STUB(__imp__IoSetShareAccess);
REX_EXPORT_STUB(__imp__IoStartNextPacket);
REX_EXPORT_STUB(__imp__IoStartNextPacketByKey);
REX_EXPORT_STUB(__imp__IoStartPacket);
REX_EXPORT_STUB(__imp__IoSynchronousFsdRequest);
REX_EXPORT_STUB(__imp__NtDeleteFile);
REX_EXPORT_STUB(__imp__NtSetSystemTime);
REX_EXPORT_STUB(__imp__NtWriteFileGather);
REX_EXPORT_STUB(__imp__IoAcquireCancelSpinLock);
REX_EXPORT_STUB(__imp__IoReleaseCancelSpinLock);
REX_EXPORT_STUB(__imp__NtCancelIoFile);
REX_EXPORT_STUB(__imp__NtCancelIoFileEx);
REX_EXPORT_STUB(__imp__SvodCreateDevice);

REX_EXPORT_STUB(__imp__SataCdRomRecordReset);
