/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/filesystem/devices/host_path_entry.h>
#include <rex/filesystem/devices/host_path_file.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

/*
 * Size of the read-ahead window, in KB. 0 turns it off.
 *
 * 256 KB rather than 64 KB. With 64 KB the entry threshold was 16 KB (window/4), and the game does
 * not do a single read of 16 KB or less: its average is 375 KB and the smallest one it repeats in a
 * loop is 64 KB (measured with 64 KB: 5 hits out of 1,232 reads). With 256 KB the threshold lands
 * exactly on those 64 KB. The RAM ceiling goes down, not up: 16 windows x 256 KB = 4 MB, against
 * 128 x 64 KB = 8 MB.
 */
REXCVAR_DEFINE_INT32(nfsmw_io_ventana_kb, 256, "Filesystem",
                     "Lectura anticipada por fichero, en KB (0 = apagada). Solo datos de solo lectura.");

// How many windows can exist at once. A hard cap so that opening many files does not eat the RAM.
// 128 was an arbitrary number; in a whole measured session there were never more than 10 live windows.
REXCVAR_DEFINE_INT32(nfsmw_io_ventanas_max, 16, "Filesystem",
                     "Maximo de ficheros con lectura anticipada a la vez.");

/*
 * Split large direct reads into pieces of this many MB. 0 = a single call.
 *
 * The worst case measured is a pread() of 19,932,032 bytes that takes 261.5 ms (there are 7 reads
 * over 8 MB, 169.2 ms on average). During those 261 ms the thread passes no point where the
 * scheduler can cleanly switch it out, and the per-file lock of XFile (file_lock_) stays held the
 * whole time.
 *
 * The cost of splitting can be bounded with the same data: fitting time = fixed + bytes/rate over
 * the 4.00 MB reads (59.8 ms on average, 21 samples) and the 19.01 MB reads (275.6 ms, 2 samples)
 * gives rate = 69.5 MB/s and fixed = 2.3 ms per call. So splitting 19 MB into 4 MB pieces is 4
 * extra calls = +9 ms over 275 (+3 %); 1 MB pieces would be 18 extra = +41 ms (+15 %). Hence 4
 * and not 1.
 *
 * In exchange, the worst pread drops from 261 ms to about 60, and short reads from the file system
 * are no longer visible: the loop keeps asking from where it stopped and only stops when a piece
 * returns 0 bytes (real end of file). The unsplit path returned the short read as it was.
 *
 * Measured: this does not raise FPS. Reads run on a thread other than the render thread, and outside
 * loading screens there is no correlation between slow reads and stutters. It is on because it
 * bounds the worst case and fixes short reads, not because a gain is expected.
 */
REXCVAR_DEFINE_INT32(nfsmw_io_trozo_mb, 4, "Filesystem",
                     "Parte las lecturas directas grandes en trozos de estos MB (0 = de una sola vez).");

/*
 * RAM read cache by blocks (nfsmw_io_cache_mb, off). The game rereads the same data from the SD
 * every lap.
 *
 * Measured over 5 minutes of racing (line `[io] rangos:`):
 *     from minute 2 on, between 79 % and 95 % of the bytes the game reads from disk had already
 *     been read before, and the sizes repeat with the lap period (20.9 MB three times, 13.0-13.6 MB
 *     twice...).
 * NFSMW streaming works per zone pack (eStreamingPack): on leaving a zone it releases the whole
 * pack and on entering again it asks for it again. We evict nothing (`0 desalojos` in every
 * report): the one releasing it is the game's memory manager, and it cannot be told no.
 *
 * What can be done is make the second time not cost a trip to the SD. In a race that is 441
 * reads, 181.5 MB and 4,447 ms inside read(), with single reads of up to 132 ms: until the pack
 * arrives, the facade shows the fallback texture, which is the "finishes loading when I get
 * close" effect seen on the console.
 *
 * 256 KB blocks with LRU. Missing blocks are read grouped, in a single pread per contiguous range,
 * so the first read costs the same as without the cache (not 36 calls for one of 9 MB) and later
 * ones are memcpy. Only read-only devices and files opened without write access: the game data
 * never changes.
 *
 * Sized at 64 MB: the unique bytes of a whole race are ~36 MB (181 MB with 80 % rereads), so the
 * whole circuit fits. 0 turns it off.
 *
 * ================================================================================================
 *  Off: on the console it does the opposite of what it promises.
 *
 *  Its own report on the console:
 *      304.6 MB delivered, 318.3 read from the SD, 1,053 blocks evicted
 *  It reads more disk than it saves. On the PC it gave 254 delivered against 217 read, which was
 *  misleading. The difference: on the console the race asks for more unique data than fits, the LRU
 *  evicts nonstop, and each 256 KB block is brought in to serve a small request and thrown away
 *  before it is reused. The 256 KB amplification per request eats the savings and more.
 *
 *  It also blocks: `g_cache_mutex` is a single mutex for all files and is held during the disk
 *  read. Any other reading thread waits behind it. (Careful when measuring: that time does not
 *  show in `[io] 15 s`, which only times inside read(); that line showed 32 ms per 15 s while the
 *  game crawled.)
 * ================================================================================================
 */
REXCVAR_DEFINE_INT32(nfsmw_io_cache_mb, 0, "Filesystem",
                     "Cache en RAM de lo ya leido del disco, en MB (0 = apagada). Solo datos de solo "
                     "lectura. El juego relee cada vuelta lo mismo.");

/*
 * Read cache by exact range. This is the second attempt, not the block cache above.
 *
 * What was measured. Across two different builds the per-lap stutter pattern is identical: a period
 * of 62-68 s, three or four fixed places, always in the same order and with the same size. In those
 * frames the breakdown says GPU 27-29 ms (normal) and the `grabar` stage 0.2-0.4 ms: the time is
 * neither in the GPU nor in recording. What does go together with the big stutters are `[io] LENTO`
 * reads of NFS\ZZDATA6.BIN marked (RELEIDA), of 11 to 20 ms, and the worst one of the session
 * blocks for 132 to 140 ms. They are rereads of the zone pack: the game releases the pack on
 * leaving and asks for it again on entering, with the same offset and the same size.
 *
 * Why by range and not by blocks. The block cache above (nfsmw_io_cache_mb) failed on the console
 * because of the blocks: only 1 % of the offsets the game asks for are aligned to 256 KB, so it
 * brought 365 KB from the SD for every 110 KB requested (x3.3 amplification) and ended up reading
 * more disk than it saved. Storing the exact range (file, offset, bytes) makes the amplification
 * zero by construction: not a single byte the game did not ask for is read.
 *
 * 4 MB floor (first sizing; lowered below). Only requests of 4 MB or more got in. That leaves out
 * entirely the small requests, the ones that sank the previous attempt, and keeps exactly the
 * spikes the player notices.
 *
 * The numbers, simulated on the session logs: the slowest read of the race goes from 132.2 ms to
 * 29, and the 12 spikes of 80 ms or more go to 0. It costs 20.9 MB, 3 entries and 0 evictions.
 *
 * The cap cannot be lowered. With a 20 MB cap the result is zero spikes avoided: the three entries
 * do not fit, the LRU evicts them before they are reused and not a single hit is scored. It is a
 * cliff, not a slope, and it is exactly what sank the block cache. 32 MB and no less.
 *
 * Where that RAM comes from: the host heap (1,024 MB, runtime_switch.cpp), of which the guest
 * backing eats 506. Not the GPU budget, which is separate. That is why the first sizing stopped
 * at 32.
 *
 * The lock is not held across the disk read. The other failure of the block cache was that
 * g_cache_mutex, a single mutex for all files, stayed held inside the pread: any other reading
 * thread waited behind it, and that time did not show in `[io] 15 s` (which only times inside
 * read()). Here the lock is taken to look up the table and released before going to disk; the
 * data lives in a shared_ptr, so copying it needs no lock either, and an eviction cannot free the
 * buffer another thread is reading.
 */
/*
 * Resized from the race logs. The first version (cap 32, floor 4 MB) hit 25 % and saved 10 ms
 * per race. Two misreadings of the data:
 *
 * 1. The 34 evictions were not race data: they were the sequential sweep of the level load
 *    (34 blocks of ZZDATA0.BIN, 4 MB at a time, 154 MB, each read once). Raising the cap from 32
 *    to 128 did not buy a single hit. What is needed is not storing that sweep (see GuardarRango).
 * 2. The 4 MB floor did not see what repeats during a race, which is small: 62 distinct requests
 *    of 0.14 to 2.0 MB (24.7 MB in total), repeated 2 to 10 times each, almost all from
 *    ZZDATA6.BIN. Simulated on the real trace: floor 4 MB -> 16.6 % hits and 10 ms; floor
 *    256 KB -> 52.7 % and 1,132 ms saved per race. Footprint with that floor: 56.6 MB, so 64 MB
 *    is the first cap with 0 evictions, and above it nothing is gained.
 *
 * Memory: it comes from the host heap (1,024 MB), not from the GPU heap. With ~506 MB for the guest
 * and the thread stacks, 64 MB leaves margin; 128 would not in the pessimistic case.
 */
REXCVAR_DEFINE_INT32(nfsmw_io_rangos_mb, 64, "Filesystem",
                     "Cache de lecturas por rango exacto, tope total en MB (0 = apagada). 64 = la huella "
                     "medida de una carrera (56,6 MB) sin expulsiones; mas no compra nada.");

// Floor, in KB. At 4 MB it left out everything that is reread during a race (0.14-2.0 MB).
REXCVAR_DEFINE_INT32(nfsmw_io_rangos_min_kb, 256, "Filesystem",
                     "Solo se cachean las lecturas de estos KB o mas. 256 = el punto donde la simulacion "
                     "deja de ganar (con 128 suben las expulsiones y bajan los aciertos).");

// Compatibility: the old cvar in MB. If a toml sets it to a value > 0, it overrides the KB one.
REXCVAR_DEFINE_INT32(nfsmw_io_rangos_min_mb, 0, "Filesystem",
                     "OBSOLETO (build 131): usar nfsmw_io_rangos_min_kb. Si es > 0 manda sobre el de KB.");

// Per-entry ceiling: a single read cannot take more than this out of the total cap.
REXCVAR_DEFINE_INT32(nfsmw_io_rangos_max_mb, 12, "Filesystem",
                     "Ninguna entrada de la cache de rangos pasa de estos MB.");

namespace rex::filesystem {

namespace {
std::atomic<uint64_t> g_aciertos{0};
std::atomic<uint64_t> g_rellenos{0};
std::atomic<uint64_t> g_directas{0};
std::atomic<uint64_t> g_bytes_ram{0};
std::atomic<int64_t> g_ventanas_vivas{0};

// Above this it does not pay off: the request is already large and the window would only add an extra copy.
constexpr size_t kPeticionMaxFraccion = 4;  // pedido <= ventana/4

/* The read cache (see nfsmw_io_cache_mb). */
constexpr size_t kBloqueCache = 256 * 1024;

struct BloqueCache {
  std::vector<uint8_t> datos;  // may be shorter than kBloqueCache at end of file
  uint64_t uso = 0;            // for the LRU
};

std::mutex g_cache_mutex;
std::unordered_map<uint64_t, BloqueCache> g_cache;   // clave: id de fichero << 32 | bloque
std::map<uint64_t, uint64_t> g_cache_lru;            // use -> key, oldest first
std::unordered_map<std::string, uint32_t> g_cache_ids;  // path -> id, so the key is exact
uint64_t g_cache_bytes = 0;
uint64_t g_cache_uso = 0;
uint64_t g_cache_aciertos = 0;
uint64_t g_cache_fallos = 0;
uint64_t g_cache_bytes_servidos = 0;
uint64_t g_cache_bytes_disco = 0;
uint64_t g_cache_expulsados = 0;
uint64_t g_cache_informe = 0;
bool g_cache_sin_memoria = false;  // turned itself off for lack of RAM: not retried

// Stable id per path, assigned the first time it is opened. With it, the block key cannot
// collide between files (a 64-bit hash could, and a collision would serve another file's data).
uint32_t IdDeRuta(const std::string& ruta) {
  std::lock_guard lock(g_cache_mutex);
  const auto [it, nuevo] = g_cache_ids.try_emplace(ruta, uint32_t(g_cache_ids.size() + 1));
  return it->second;
}

// Marks a block as just used. Called with g_cache_mutex held.
void TocarBloque(uint64_t clave, BloqueCache& bloque) {
  if (bloque.uso) {
    g_cache_lru.erase(bloque.uso);
  }
  bloque.uso = ++g_cache_uso;
  g_cache_lru[bloque.uso] = clave;
}

// Brings the cache under the limit by dropping the oldest blocks. Called with g_cache_mutex held.
void PodarCache(size_t limite) {
  while (g_cache_bytes > limite && !g_cache_lru.empty()) {
    const auto viejo = g_cache_lru.begin();
    const auto it = g_cache.find(viejo->second);
    if (it != g_cache.end()) {
      g_cache_bytes -= it->second.datos.size();
      g_cache.erase(it);
      ++g_cache_expulsados;
    }
    g_cache_lru.erase(viejo);
  }
}

/*
 * Serves the read from RAM, bringing from disk only the missing blocks (grouped, one pread per
 * contiguous range). Returns false if it cannot: then the usual path answers and nothing that
 * matters has been touched.
 */
bool LeerConCache(FileHandle* fh, uint32_t id, std::span<uint8_t> buffer, size_t byte_offset,
                  size_t* out_bytes_read) {
  const size_t limite = static_cast<size_t>(REXCVAR_GET(nfsmw_io_cache_mb)) * 1024u * 1024u;
  const size_t pedido = buffer.size();
  if (!limite || !pedido || !id || g_cache_sin_memoria) {
    return false;
  }
  const uint64_t primero = byte_offset / kBloqueCache;
  const uint64_t ultimo = (byte_offset + pedido - 1) / kBloqueCache;
  // A request larger than half the cache would fill it on its own and evict everything useful.
  if ((ultimo - primero + 1) * kBloqueCache > limite / 2) {
    return false;
  }

  std::lock_guard lock(g_cache_mutex);
  size_t copiados = 0;
  for (uint64_t b = primero; b <= ultimo; ++b) {
    const uint64_t clave = (static_cast<uint64_t>(id) << 32) | b;
    auto it = g_cache.find(clave);
    if (it == g_cache.end()) {
      // Contiguous run of missing blocks: a single read for all of them.
      uint64_t fin = b;
      while (fin < ultimo && !g_cache.count((static_cast<uint64_t>(id) << 32) | (fin + 1))) {
        ++fin;
      }
      const size_t tramo = static_cast<size_t>(fin - b + 1) * kBloqueCache;
      size_t leidos = 0;
      /*
       * If the console does not have that RAM, the cache turns itself off and does not try again:
       * the game carries on down the usual path. It can never bring the process down for lack of memory.
       */
      try {
        std::vector<uint8_t> tmp(tramo);
        /*
         * Same as LeerDirecta: in pieces, and a short read is not end of file (the file system
         * truncates large ones). The loop only stops when a piece returns 0 bytes. Without this,
         * half-filled blocks would be cached and the game would get incomplete data.
         */
        const int32_t trozo_mb = REXCVAR_GET(nfsmw_io_trozo_mb);
        const size_t trozo = trozo_mb > 0 ? static_cast<size_t>(trozo_mb) * 1024u * 1024u : tramo;
        while (leidos < tramo) {
          size_t n = 0;
          const size_t pide = std::min(trozo, tramo - leidos);
          if (!fh->Read(b * kBloqueCache + leidos, tmp.data() + leidos, pide, &n)) {
            if (leidos == 0) {
              return false;  // real error: let the usual path answer
            }
            break;
          }
          if (n == 0) {
            break;  // fin de fichero
          }
          leidos += n;
        }
        ++g_cache_fallos;
        g_cache_bytes_disco += leidos;
        for (uint64_t k = b; k <= fin; ++k) {
          const size_t desde = static_cast<size_t>(k - b) * kBloqueCache;
          if (desde >= leidos) {
            break;  // fin de fichero
          }
          const size_t n = std::min(kBloqueCache, leidos - desde);
          const uint64_t ck = (static_cast<uint64_t>(id) << 32) | k;
          BloqueCache& nuevo = g_cache[ck];
          nuevo.datos.assign(tmp.begin() + desde, tmp.begin() + desde + n);
          g_cache_bytes += n;
          TocarBloque(ck, nuevo);
        }
      } catch (const std::bad_alloc&) {
        const uint64_t tenia = g_cache_bytes >> 20;
        g_cache_sin_memoria = true;
        g_cache.clear();
        g_cache_lru.clear();
        g_cache_bytes = 0;
        REXLOG_WARN("[io] cache en RAM: sin memoria, se apaga (iba por {} MB). El juego sigue leyendo "
                    "del disco como antes",
                    tenia);
        return false;
      }
      PodarCache(limite);
      it = g_cache.find(clave);  // the insertion may have rebuilt the table
      if (it == g_cache.end()) {
        break;  // not even the first block had bytes: end of file
      }
    } else {
      ++g_cache_aciertos;
      TocarBloque(clave, it->second);
    }
    const size_t dentro = byte_offset + copiados - static_cast<size_t>(b * kBloqueCache);
    if (dentro >= it->second.datos.size()) {
      break;  // the block ends before the requested range: end of file
    }
    const size_t n = std::min(it->second.datos.size() - dentro, pedido - copiados);
    std::memcpy(buffer.data() + copiados, it->second.datos.data() + dentro, n);
    copiados += n;
    if (it->second.datos.size() < kBloqueCache) {
      break;  // short block: no more file
    }
  }
  g_cache_bytes_servidos += copiados;
  *out_bytes_read = copiados;

  const uint64_t vueltas = g_cache_aciertos + g_cache_fallos;
  if (vueltas >= g_cache_informe + 200) {
    g_cache_informe = vueltas;
    REXLOG_INFO("[io] cache en RAM: {} bloques ({} MB de {}), {} servidas de RAM y {} del disco "
                "({:.1f} % de aciertos); {:.1f} MB entregados, {:.1f} leidos de la SD, {} bloques "
                "expulsados",
                g_cache.size(), g_cache_bytes >> 20, REXCVAR_GET(nfsmw_io_cache_mb), g_cache_aciertos,
                g_cache_fallos, vueltas ? 100.0 * double(g_cache_aciertos) / double(vueltas) : 0.0,
                double(g_cache_bytes_servidos) / 1048576.0, double(g_cache_bytes_disco) / 1048576.0,
                g_cache_expulsados);
  }
  return true;
}

/*
 * ================================================================================================
 * The exact-range cache. See nfsmw_io_rangos_mb above.
 *
 * The table is a vector and is scanned in full. This is deliberate: it was sized for a 32 MB cap
 * and a 4 MB floor per entry, where at most eight entries fit and the linear search is eight
 * integer comparisons (the current 64 MB cap and 256 KB floor allow up to 256). A hash map would
 * only add a hash function, a rehash that invalidates references and a possible collision that
 * would serve another file's data.
 * ================================================================================================
 */
struct EntradaRango {
  uint32_t id = 0;               // file (stable id per path, the same as IdDeRuta)
  uint64_t desplazamiento = 0;   // where the read starts
  uint32_t pedido = 0;           // how much the game asked for: part of the key, not a detail
  // The bytes the disk returned. It can be shorter than `pedido` if the file ends earlier; it is
  // stored as is and returned as is, which is what the disk would have answered.
  std::shared_ptr<const std::vector<uint8_t>> datos;
  uint64_t uso = 0;              // for the LRU
};

std::mutex g_rangos_mutex;
std::vector<EntradaRango> g_rangos;
uint64_t g_rangos_uso = 0;
uint64_t g_rangos_bytes = 0;
bool g_rangos_sin_memoria = false;  // turned itself off for lack of RAM: not retried

// Counters. Atomic so the periodic summary can read them without taking the lock.
std::atomic<uint64_t> g_rangos_aciertos{0};
std::atomic<uint64_t> g_rangos_fallos{0};
std::atomic<uint64_t> g_rangos_bytes_ram{0};
std::atomic<uint64_t> g_rangos_bytes_disco{0};
std::atomic<uint64_t> g_rangos_expulsiones{0};
std::atomic<uint64_t> g_rangos_entradas{0};
std::atomic<uint64_t> g_rangos_bytes_vivos{0};
// Why something is not cached. Without this the cache could not be tuned: the counter said how
// often it hit, but not whether the misses came from the floor, the ceiling or the load sweep.
std::atomic<uint64_t> g_rangos_bajo_suelo{0};     // reads skipped for being too small
std::atomic<uint64_t> g_rangos_sobre_techo{0};    // skipped for being too large
std::atomic<uint64_t> g_rangos_secuenciales{0};   // rejected as part of the load sweep
std::atomic<uint64_t> g_rangos_secuenciales_mb{0};

// Last byte read for each file, to detect the sequential sweep. Guarded by g_rangos_mutex.
std::unordered_map<uint32_t, uint64_t> g_rangos_ultimo_fin;

int64_t SueloBytes() {
  const int64_t viejo_mb = REXCVAR_GET(nfsmw_io_rangos_min_mb);
  if (viejo_mb > 0) {
    return viejo_mb * 1024 * 1024;  // an old toml sets it: honor it
  }
  return int64_t(REXCVAR_GET(nfsmw_io_rangos_min_kb)) * 1024;
}

// Does this read go into the cache? Only the size decides; the constructor already took care of
// the file being read-only when it gave (or did not give) it an id.
bool RangoElegible(size_t pedido) {
  const int64_t tope = int64_t(REXCVAR_GET(nfsmw_io_rangos_mb)) * 1024 * 1024;
  if (tope <= 0) {
    return false;
  }
  const int64_t techo = int64_t(REXCVAR_GET(nfsmw_io_rangos_max_mb)) * 1024 * 1024;
  if (int64_t(pedido) < SueloBytes()) {
    g_rangos_bajo_suelo.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (int64_t(pedido) > techo || int64_t(pedido) > tope) {
    g_rangos_sobre_techo.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  return true;
}

/*
 * The level load sweep: large reads whose offset is exactly the end of the previous read of the
 * same file. Measured: 31 consecutive blocks of ZZDATA0.BIN of exactly 4 MB, each read only once,
 * filled the LRU with garbage before the race started (the first hit came 100 s after the start).
 *
 * Bounded so as not to drop anything that is reread: it only counts as a sweep if the read is also
 * 4 MB or more. What the game rereads during a race is between 0.14 and 2.0 MB, so it never falls
 * here. The end of every eligible read is recorded (including the first read of the sweep, which
 * is stored: it is 1 block out of 31 and gets evicted on its own).
 */
constexpr uint64_t kBarridoMinimo = uint64_t(4) << 20;

bool EsBarridoYAnotar(uint32_t id, uint64_t desplazamiento, uint32_t pedido) {
  // Llamar con g_rangos_mutex cogido.
  auto it = g_rangos_ultimo_fin.find(id);
  const bool seguida = it != g_rangos_ultimo_fin.end() && it->second == desplazamiento;
  g_rangos_ultimo_fin[id] = desplazamiento + pedido;
  return seguida && pedido >= kBarridoMinimo;
}

/*
 * Looks up the exact range. Returns the buffer, or nullptr if it was not there.
 *
 * It returns a shared_ptr on purpose: the caller releases the lock before copying the megabytes,
 * and while it copies another thread may evict this entry. With the shared_ptr that only removes
 * the entry from the table; the bytes stay alive until the last reader releases them.
 */
std::shared_ptr<const std::vector<uint8_t>> BuscarRango(uint32_t id, uint64_t desplazamiento,
                                                        uint32_t pedido) {
  std::lock_guard lock(g_rangos_mutex);
  for (auto& e : g_rangos) {
    if (e.id == id && e.desplazamiento == desplazamiento && e.pedido == pedido) {
      e.uso = ++g_rangos_uso;
      return e.datos;
    }
  }
  return nullptr;
}

/*
 * Stores what the disk just returned. Called after the read, with the lock released for the whole
 * trip to the SD.
 *
 * The copy is done outside the lock: it is up to 12 MB of memcpy plus a heap allocation, and
 * neither has any reason to block another thread that only wants to look at the table.
 */
void GuardarRango(uint32_t id, uint64_t desplazamiento, uint32_t pedido, const uint8_t* datos,
                  size_t bytes) {
  if (!bytes) {
    return;
  }
  {
    std::lock_guard lock(g_rangos_mutex);
    if (g_rangos_sin_memoria) {
      return;
    }
    // The load sweep does not go in. Decided here, before copying the megabytes.
    if (EsBarridoYAnotar(id, desplazamiento, pedido)) {
      g_rangos_secuenciales.fetch_add(1, std::memory_order_relaxed);
      g_rangos_secuenciales_mb.fetch_add(bytes, std::memory_order_relaxed);
      return;
    }
  }

  std::shared_ptr<std::vector<uint8_t>> copia;
  try {
    copia = std::make_shared<std::vector<uint8_t>>(datos, datos + bytes);
  } catch (const std::bad_alloc&) {
    // If the console does not have that RAM, the cache turns off and is not retried. The game keeps
    // reading from disk as usual: it can never bring the process down for lack of memory.
    uint64_t tenia = 0;
    {
      std::lock_guard lock(g_rangos_mutex);
      tenia = g_rangos_bytes >> 20;
      g_rangos_sin_memoria = true;
      g_rangos.clear();
      g_rangos_bytes = 0;
    }
    g_rangos_entradas.store(0, std::memory_order_relaxed);
    g_rangos_bytes_vivos.store(0, std::memory_order_relaxed);
    REXLOG_WARN("[io] rangos-cache: sin memoria, se apaga (iba por {} MB). El juego sigue leyendo "
                "del disco como antes",
                tenia);
    return;
  }

  const size_t tope = size_t(REXCVAR_GET(nfsmw_io_rangos_mb)) * 1024u * 1024u;
  std::lock_guard lock(g_rangos_mutex);
  for (const auto& e : g_rangos) {
    if (e.id == id && e.desplazamiento == desplazamiento && e.pedido == pedido) {
      return;  // another thread asked for the same range and got there first: its copy is just as good
    }
  }
  EntradaRango& nueva = g_rangos.emplace_back();
  nueva.id = id;
  nueva.desplazamiento = desplazamiento;
  nueva.pedido = pedido;
  nueva.datos = std::move(copia);
  nueva.uso = ++g_rangos_uso;
  g_rangos_bytes += bytes;

  // LRU: evict the oldest until it fits. The entry just added has the highest use count, so it is
  // never the one evicted.
  while (g_rangos_bytes > tope && g_rangos.size() > 1) {
    size_t viejo = 0;
    for (size_t i = 1; i < g_rangos.size(); ++i) {
      if (g_rangos[i].uso < g_rangos[viejo].uso) {
        viejo = i;
      }
    }
    g_rangos_bytes -= g_rangos[viejo].datos->size();
    g_rangos.erase(g_rangos.begin() + static_cast<ptrdiff_t>(viejo));
    g_rangos_expulsiones.fetch_add(1, std::memory_order_relaxed);
  }
  g_rangos_entradas.store(g_rangos.size(), std::memory_order_relaxed);
  g_rangos_bytes_vivos.store(g_rangos_bytes, std::memory_order_relaxed);
}
}  // namespace

EstadisticasRangos LeerEstadisticasRangos() {
  EstadisticasRangos e;
  e.aciertos = g_rangos_aciertos.load(std::memory_order_relaxed);
  e.fallos = g_rangos_fallos.load(std::memory_order_relaxed);
  e.bytes_ram = g_rangos_bytes_ram.load(std::memory_order_relaxed);
  e.bytes_disco = g_rangos_bytes_disco.load(std::memory_order_relaxed);
  e.entradas = g_rangos_entradas.load(std::memory_order_relaxed);
  e.bytes_vivos = g_rangos_bytes_vivos.load(std::memory_order_relaxed);
  e.expulsiones = g_rangos_expulsiones.load(std::memory_order_relaxed);
  e.bajo_suelo = g_rangos_bajo_suelo.load(std::memory_order_relaxed);
  e.sobre_techo = g_rangos_sobre_techo.load(std::memory_order_relaxed);
  e.secuenciales = g_rangos_secuenciales.load(std::memory_order_relaxed);
  e.secuenciales_bytes = g_rangos_secuenciales_mb.load(std::memory_order_relaxed);
  e.suelo_kb = uint64_t(SueloBytes() / 1024);
  const int32_t tope = REXCVAR_GET(nfsmw_io_rangos_mb);
  e.tope_mb = tope > 0 ? uint64_t(tope) : 0;
  {
    std::lock_guard lock(g_rangos_mutex);
    e.sin_memoria = g_rangos_sin_memoria;
  }
  return e;
}

EstadisticasVentana LeerEstadisticasVentana() {
  EstadisticasVentana e;
  e.aciertos = g_aciertos.load(std::memory_order_relaxed);
  e.rellenos = g_rellenos.load(std::memory_order_relaxed);
  e.directas = g_directas.load(std::memory_order_relaxed);
  e.bytes_ram = g_bytes_ram.load(std::memory_order_relaxed);
  int64_t vivas = g_ventanas_vivas.load(std::memory_order_relaxed);
  e.ventanas_vivas = vivas > 0 ? static_cast<uint64_t>(vivas) : 0;
  return e;
}

HostPathFile::HostPathFile(uint32_t file_access, HostPathEntry* entry,
                           std::unique_ptr<rex::filesystem::FileHandle> file_handle)
    : File(file_access, entry), file_handle_(std::move(file_handle)) {
  // The window is decided once, at open time. Conditions: there is a handle (it is not a directory),
  // the device is read-only, the file was not opened for writing and the cvar allows it.
  const int32_t kb = REXCVAR_GET(nfsmw_io_ventana_kb);
  const bool quiere_escribir =
      (file_access & (FileAccess::kGenericWrite | FileAccess::kFileWriteData |
                      FileAccess::kFileAppendData | FileAccess::kGenericAll)) != 0;
  /*
   * The RAM cache, with the same conditions as the window except that it has its own cvar. The id
   * is requested once, at open time, so the path map is not touched on every read.
   *
   * The same id serves both caches, the block one (off) and the range one. That the device is
   * read-only and the file was not opened for writing is checked here and only here: that is why
   * neither cache can ever touch a save or a profile, where the bytes change underneath us and
   * serving a stale copy would mean a corrupt save.
   */
  if (file_handle_ && entry && entry->is_read_only() && !quiere_escribir &&
      (REXCVAR_GET(nfsmw_io_cache_mb) > 0 || REXCVAR_GET(nfsmw_io_rangos_mb) > 0)) {
    cache_id_ = IdDeRuta(entry->path());
  }
  if (file_handle_ && kb > 0 && entry && entry->is_read_only() && !quiere_escribir) {
    const int64_t max = REXCVAR_GET(nfsmw_io_ventanas_max);
    if (g_ventanas_vivas.fetch_add(1, std::memory_order_relaxed) < max) {
      ventana_tam_ = static_cast<size_t>(kb) * 1024u;
      ventana_activa_ = true;
      ventana_contada_ = true;
    } else {
      g_ventanas_vivas.fetch_sub(1, std::memory_order_relaxed);
    }
  }
}

HostPathFile::~HostPathFile() {
  if (ventana_contada_) {
    g_ventanas_vivas.fetch_sub(1, std::memory_order_relaxed);
  }
}

void HostPathFile::Destroy() {
  delete this;
}

// fsync on POSIX, FlushFileBuffers on Windows. Called by NtFlushBuffersFile, which the game uses right
// after saving; without it, the save could be left half written on the SD if the game was closed
// right afterwards.
X_STATUS HostPathFile::Flush() {
  if (!file_handle_) {
    return X_STATUS_SUCCESS;
  }
  file_handle_->Flush();
  return X_STATUS_SUCCESS;
}

bool HostPathFile::RellenarVentana(size_t byte_offset, size_t pedido, std::span<uint8_t> buffer,
                                   size_t* out_bytes_read, X_STATUS* out_status) {
  if (ventana_.size() != ventana_tam_) {
    ventana_.resize(ventana_tam_);
  }

  size_t leidos = 0;
  ++ventana_rellenos_;
  g_rellenos.fetch_add(1, std::memory_order_relaxed);
  if (!file_handle_->Read(byte_offset, ventana_.data(), ventana_tam_, &leidos)) {
    // Real error. The window is turned off and the normal path answers.
    ventana_bytes_ = 0;
    ventana_activa_ = false;
    ventana_.clear();
    ventana_.shrink_to_fit();
    return false;
  }

  // What was read is valid whether or not it covers the request: they are real bytes of the file and
  // the file is read-only.
  ventana_inicio_ = byte_offset;
  ventana_bytes_ = leidos;

  if (leidos < pedido) {
    /*
     * The window does not cover the request. It can be the end of the file (and then the direct read
     * will return the same) or a short read from the file system, which cannot be told apart here. In
     * both cases the answer that cannot be wrong is the usual one: reply with the direct read. If this
     * happens several times in a row, the file system is truncating large reads, and the window would
     * only add an extra trip per read: it is turned off for this file.
     */
    if (++ventana_cortas_ >= 4) {
      ventana_activa_ = false;
      ventana_bytes_ = 0;
      ventana_.clear();
      ventana_.shrink_to_fit();
    }
    return false;
  }
  ventana_cortas_ = 0;

  std::memcpy(buffer.data(), ventana_.data(), pedido);
  *out_bytes_read = pedido;
  *out_status = X_STATUS_SUCCESS;
  g_bytes_ram.fetch_add(pedido, std::memory_order_relaxed);
  return true;
}

/*
 * The direct read, split into pieces. See nfsmw_io_trozo_mb above.
 *
 * It must return exactly the same as a single pread in every case the game can see: success with
 * whatever count results, and X_STATUS_END_OF_FILE only if the first call really fails.
 */
X_STATUS HostPathFile::LeerDirecta(std::span<uint8_t> buffer, size_t byte_offset,
                                   size_t* out_bytes_read) {
  const size_t pedido = buffer.size();
  const int32_t trozo_mb = REXCVAR_GET(nfsmw_io_trozo_mb);
  const size_t trozo = trozo_mb > 0 ? static_cast<size_t>(trozo_mb) * 1024u * 1024u : 0;

  if (!trozo || pedido <= trozo) {
    if (file_handle_->Read(byte_offset, buffer.data(), pedido, out_bytes_read)) {
      return X_STATUS_SUCCESS;
    }
    *out_bytes_read = 0;
    return X_STATUS_END_OF_FILE;
  }

  size_t total = 0;
  while (total < pedido) {
    const size_t n = std::min(trozo, pedido - total);
    size_t leidos = 0;
    if (!file_handle_->Read(byte_offset + total, buffer.data() + total, n, &leidos)) {
      // Real error. If there were already good bytes they are delivered; if not, it is the usual failure.
      if (total == 0) {
        *out_bytes_read = 0;
        return X_STATUS_END_OF_FILE;
      }
      break;
    }
    if (leidos == 0) {
      // End of file. A short but non-empty piece does not stop the loop: it asks again from where it
      // stopped, which is better than a single pread (that returned the short read and nothing more).
      break;
    }
    total += leidos;
  }

  *out_bytes_read = total;
  return X_STATUS_SUCCESS;
}

X_STATUS HostPathFile::ReadSync(std::span<uint8_t> buffer, size_t byte_offset,
                                size_t* out_bytes_read) {
  // No handle means a directory (see HostPathEntry::Open). A directory is enumerated, not read.
  if (!file_handle_) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  if (!(file_access_ & (FileAccess::kGenericRead | FileAccess::kFileReadData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  const size_t pedido = buffer.size();
  if (ventana_activa_ && pedido && pedido <= ventana_tam_ / kPeticionMaxFraccion) {
    // The request falls entirely within what is already in RAM.
    if (ventana_bytes_ && byte_offset >= ventana_inicio_ &&
        byte_offset + pedido <= ventana_inicio_ + ventana_bytes_) {
      std::memcpy(buffer.data(), ventana_.data() + (byte_offset - ventana_inicio_), pedido);
      *out_bytes_read = pedido;
      ++ventana_aciertos_;
      g_aciertos.fetch_add(1, std::memory_order_relaxed);
      g_bytes_ram.fetch_add(pedido, std::memory_order_relaxed);
      return X_STATUS_SUCCESS;
    }

    X_STATUS estado = X_STATUS_SUCCESS;
    const bool servida = RellenarVentana(byte_offset, pedido, buffer, out_bytes_read, &estado);

    /*
     * Audit. Each fill is a 256 KB read from the SD that only pays off if hits follow. If after
     * 8 fills this file has not given at least one hit per fill, the game is not reading it
     * sequentially and the window is pure cost: it is turned off for this file. This prevents a
     * repeat of the measured case where 4.1 MB of SD reads served 0.3 MB without anyone noticing.
     */
    if (ventana_activa_ && ventana_rellenos_ >= 8 && ventana_aciertos_ < ventana_rellenos_) {
      ventana_activa_ = false;
      ventana_bytes_ = 0;
      ventana_.clear();
      ventana_.shrink_to_fit();
    }

    if (servida) {
      return estado;
    }
    // If it could not, it falls through to the usual path below.
  }

  /*
   * The exact-range cache, before going to disk. See nfsmw_io_rangos_mb.
   *
   * It is the only thing between the game and the SD for large reads: the zone pack that the game
   * releases on leaving and asks for again on entering, byte for byte, at the same place and with
   * the same size. Those rereads are the ones that block for 11 to 140 ms in the middle of a lap.
   */
  const bool rango_elegible = cache_id_ && RangoElegible(pedido);
  if (rango_elegible) {
    // The lock only lasts as long as the table lookup; the memcpy happens outside it, and the
    // shared_ptr keeps the data alive even if another thread evicts this entry while we copy.
    if (auto datos = BuscarRango(cache_id_, byte_offset, uint32_t(pedido))) {
      const size_t n = std::min(datos->size(), pedido);
      std::memcpy(buffer.data(), datos->data(), n);
      *out_bytes_read = n;
      g_rangos_aciertos.fetch_add(1, std::memory_order_relaxed);
      g_rangos_bytes_ram.fetch_add(n, std::memory_order_relaxed);
      return X_STATUS_SUCCESS;
    }
  }

  // The RAM cache, before going to disk. See nfsmw_io_cache_mb: the game rereads the same data every
  // lap, and those reads are what make a facade take a while to get its proper texture.
  if (cache_id_ && pedido) {
    size_t leidos = 0;
    if (LeerConCache(file_handle_.get(), cache_id_, buffer, byte_offset, &leidos)) {
      *out_bytes_read = leidos;
      return X_STATUS_SUCCESS;
    }
  }

  g_directas.fetch_add(1, std::memory_order_relaxed);
  const X_STATUS estado = LeerDirecta(buffer, byte_offset, out_bytes_read);

  /*
   * And here what just arrived is stored. None of this happens with a shared lock held: the read
   * above has already finished. That exact mistake (g_cache_mutex held inside the pread) is what
   * made the block cache block the other threads without showing in any counter.
   */
  if (rango_elegible && estado == X_STATUS_SUCCESS && *out_bytes_read) {
    g_rangos_fallos.fetch_add(1, std::memory_order_relaxed);
    g_rangos_bytes_disco.fetch_add(*out_bytes_read, std::memory_order_relaxed);
    GuardarRango(cache_id_, byte_offset, uint32_t(pedido), buffer.data(), *out_bytes_read);
  }
  return estado;
}

X_STATUS HostPathFile::WriteSync(std::span<const uint8_t> buffer, size_t byte_offset,
                                 size_t* out_bytes_written) {
  if (!file_handle_) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  if (!(file_access_ &
        (FileAccess::kGenericWrite | FileAccess::kFileWriteData | FileAccess::kFileAppendData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  // Safety belt: a file being written cannot have a window. It should never get here with one (it is
  // only enabled on read-only devices and without write access), but if it did, the window is dropped.
  ventana_activa_ = false;
  ventana_bytes_ = 0;

  if (file_handle_->Write(byte_offset, buffer.data(), buffer.size(), out_bytes_written)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

X_STATUS HostPathFile::SetLength(size_t length) {
  if (!file_handle_) {
    return X_STATUS_FILE_IS_A_DIRECTORY;
  }
  if (!(file_access_ & (FileAccess::kGenericWrite | FileAccess::kFileWriteData))) {
    return X_STATUS_ACCESS_DENIED;
  }

  ventana_activa_ = false;
  ventana_bytes_ = 0;

  if (file_handle_->SetLength(length)) {
    return X_STATUS_SUCCESS;
  } else {
    return X_STATUS_END_OF_FILE;
  }
}

}  // namespace rex::filesystem
