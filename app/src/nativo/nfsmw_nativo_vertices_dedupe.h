// nfsmw - native renderer: deduplication of vertex uploads within a frame.
//
// The problem, measured
// ---------------------
// Every draw copies to the upload buffer the [vmin..vmax] range of each of its vertex bindings,
// with the byte order swapped. There is no cache: the same piece of geometry is copied again in
// full every time it is drawn.
//
//   Console, last 10 s stretch of a race (nfsmw_001.log):
//       vertices 2044.9 MB in 269 frames = 7.60 MB per frame = 204 MB/s
//       CPU write speed into the upload buffer's memory type (type 1, no CPU cache,
//       coherent) = 2462 MB/s  ->  3.09 ms per frame just writing
//
//   PC, with the nfsmw_nativo_diag_vertices_repetidos diagnostic on, steady race:
//       "5954.9 MB copiados; 2584.2 MB repetidos en el mismo fotograma y
//        3329.2 MB iguales a un fotograma anterior"
//       -> 99.0-99.6 % of the bytes are byte-for-byte repeats, in eight reports in a row.
//          Of those, 41-49 % repeat within the same frame.
//
// What this piece does
// --------------------
// Only the safe half: the same-frame one. A table from (physical address, bytes, order) to the
// offset that copy already occupies in this frame's upload buffer. If a later draw asks for
// exactly the same range with the same order, the existing offset is bound and nothing is copied.
//
// Why it is exact and not a gamble
// --------------------------------
// On the Xbox 360 the GPU reads vertices from main memory when it executes the draw, not when
// the game records the packet. A title cannot rewrite a vertex range it has already referenced
// in this frame without first synchronizing with the GPU: if it did, the result on the original
// console would not be defined either. So between two draws of the same frame with no
// synchronization in between, the contents of that range are the same by construction.
//
// The points where the guest may have rewritten memory are the synchronizations. Olvidar() is
// called there and the table is cleared. With that, deduplication does not rely on any
// assumption.
//
// What it does not do (on purpose)
// --------------------------------
// The other half (the 51-59 % that is equal to an earlier frame) needs a persistent buffer and
// a content check (XXH3 of the guest bytes, as the texture cache does with huella_cruda). It is
// more expensive and more delicate than this half, which is free.
//
// How it is wired in (nfsmw_nativo_dibujos.cpp)
// ---------------------------------------------
//   - UsarRanura(...)      : dedupe_.NuevoFotograma(fotograma_);   // after ++fotograma_
//       This already covers EnviarYEsperar: despite its name it does not wait for anything, it
//       switches work slots, and that goes through UsarRanura, which resets subida_usado_ and
//       bumps fotograma_. Old offsets die on their own.
//   - dedupe_.Olvidar() where the ring thread serves a guest wait for the GPU (a fence or a
//     WAIT_REG_MEM): the only point where the game can legally rewrite a range it already drew.
//
//   Beware of nfsmw_nativo_enviar_tras_sombras: when true, the shadow pass ends in one slot and
//   the scene in the next, so the scene cannot reuse the shadow copies and half the savings
//   are lost.
//   - in the bindings loop of Dibujar, before Reservar/EncolarCopia:
//         VkDeviceSize offset;
//         if (dedupe_.Buscar(origen.direccion, origen.bytes, uint32_t(origen.orden), offset)) {
//           offsets_vertices[b] = offset;
//           continue;                       // already in this frame's upload buffer
//         }
//         Reservar(origen.bytes, 4, offset);
//         ... (the usual copy) ...
//         dedupe_.Anotar(origen.direccion, origen.bytes, uint32_t(origen.orden), offset);
//
// The table has kRanuras entries of 40 bytes (640 KB) and is cleared by generation, without
// memset.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nfsmw::nativo {

// Deduplication of vertex bindings within a frame. Single thread (the ring thread).
class DedupeVertices {
 public:
  // Number of slots. Power of 2. With ~1,244 draws per frame, each with 1 or 2 vertex bindings,
  // they fit easily in 4096 slots. The colisiones() counter tells whether it falls short: if it
  // rises, double it here.
  /*
   * Raised to 16384 (640 KB). With 4096 there were 19,705-33,121 collisions per report; with
   * 16384 they drop to 5,423-6,725, 3.6x fewer.
   *
   * But the savings do not go up (30 % -> 31 %, 1.83 -> 1.84 MB per frame). Collisions were not
   * the limit. The real ceiling of the safe half is ~30-38 % of the bytes, not the 41-49 % the
   * diagnostic measures, because the table is cleared on every slot change (NuevoFotograma,
   * which happens several times per frame with EnviarYEsperar) and on every WAIT_REG_MEM. Those
   * clears are exactly what makes this exact rather than a gamble.
   *
   * It stays at 16384 because evicting live entries is wasted work, but enlarging it further
   * will not bring more savings.
   */
  static constexpr size_t kRanuras = 16384;

  // A frame starts: everything recorded before becomes invalid (the upload buffer has been reset).
  void NuevoFotograma(uint64_t fotograma) {
    // fotograma_ can never equal kVacia, which marks an unused entry.
    fotograma_ = fotograma == kVacia ? 0 : fotograma;
  }

  // The guest may have synchronized with the GPU and rewritten memory: what was recorded
  // becomes invalid even within the same frame. O(1): it only moves the generation.
  void Olvidar() {
    ++generacion_;
    ++olvidos_;
  }

  // true if that range is already copied in this frame's upload buffer. 'offset' then holds
  // its offset and nothing needs to be copied.
  //
  // huella: 0 = no check (the normal case). Non-zero = paranoid mode: the content fingerprint
  // is stored when recording and compared when looking up; on a mismatch it is counted in
  // discrepancias() and false is returned (the data is copied as usual). It proves in a real
  // race that deduplication never serves stale data, without risking a single pixel.
  bool Buscar(uint64_t direccion, uint32_t bytes, uint32_t orden, uint64_t& offset,
              uint64_t huella = 0) {
    ++consultas_;
    const uint64_t clave = Clave(direccion, orden);
    const Entrada& e = tabla_[Indice(clave, bytes)];
    if (e.fotograma != fotograma_ || e.generacion != generacion_ || e.clave != clave ||
        e.bytes != bytes) {
      return false;
    }
    if (huella != 0 && e.huella != huella) {
      ++discrepancias_;
      return false;
    }
    ++aciertos_;
    bytes_ahorrados_ += bytes;
    offset = e.offset;
    return true;
  }

  // Records a copy just made. 'huella' is only used in the paranoid mode of Buscar.
  void Anotar(uint64_t direccion, uint32_t bytes, uint32_t orden, uint64_t offset,
              uint64_t huella = 0) {
    const uint64_t clave = Clave(direccion, orden);
    Entrada& e = tabla_[Indice(clave, bytes)];
    if (e.fotograma == fotograma_ && e.generacion == generacion_ &&
        (e.clave != clave || e.bytes != bytes)) {
      ++colisiones_;  // another live entry loses its slot: the table is too small
    }
    e.clave = clave;
    e.bytes = bytes;
    e.offset = offset;
    e.huella = huella;
    e.fotograma = fotograma_;
    e.generacion = generacion_;
  }

  // Counters for the C6 report (deltas between reports, like the others).
  uint64_t consultas() const { return consultas_; }
  uint64_t aciertos() const { return aciertos_; }
  uint64_t bytes_ahorrados() const { return bytes_ahorrados_; }
  uint64_t colisiones() const { return colisiones_; }
  uint64_t olvidos() const { return olvidos_; }
  uint64_t discrepancias() const { return discrepancias_; }

 private:
  static constexpr uint64_t kVacia = ~uint64_t(0);

  struct Entrada {
    uint64_t clave = 0;           // (direccion << 2) | orden
    uint64_t offset = 0;          // offset inside the upload buffer
    uint64_t huella = 0;          // paranoid mode only
    uint64_t fotograma = kVacia;  // kVacia = never used
    uint32_t bytes = 0;
    uint32_t generacion = 0;      // incremented by every Olvidar()
  };
  static_assert(sizeof(Entrada) == 40, "la entrada tiene que caber en 40 bytes");

  static uint64_t Clave(uint64_t direccion, uint32_t orden) {
    return (direccion << 2) | (uint64_t(orden) & 0x3);
  }

  // Multiply-and-shift mix (splitmix). Cheap, and it spreads aligned addresses well, which is
  // exactly what arrives here.
  static size_t Indice(uint64_t clave, uint32_t bytes) {
    uint64_t x = clave ^ (uint64_t(bytes) * 0x9E3779B97F4A7C15ull);
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    return size_t(x) & (kRanuras - 1);
  }

  std::array<Entrada, kRanuras> tabla_{};
  uint64_t fotograma_ = 0;
  uint32_t generacion_ = 0;

  uint64_t consultas_ = 0;
  uint64_t aciertos_ = 0;
  uint64_t bytes_ahorrados_ = 0;
  uint64_t colisiones_ = 0;
  uint64_t olvidos_ = 0;
  uint64_t discrepancias_ = 0;
};

/*
 * The other half: vertex ranges equal to an earlier frame (fh2_nativo_cache_vertices).
 *
 * A persistent buffer that UsarRanura does not reset holds copies of vertex ranges of 4 KB or more. A copy is reused
 * in later frames while the guest has not written its pages since it was made. That is known from the write watch
 * (Memory::EnablePhysicalMemoryAccessCallbacks): every write the guest makes to a watched page reaches
 * EscrituraVigilada, which stamps the page with a new tick. A copy remembers the tick from before its watch was
 * enabled; if any of its pages has a newer one, it is stale and the range is copied again, to a new place.
 *
 * Nothing is ever written where the GPU may still read. The buffer is split into segments; a segment is only reused
 * (and its copies dropped) once the GPU has finished the last submission that used it. Submissions are numbered at
 * UsarRanura: a work slot is only reused after its fence (DestinosVulkan::Grabar -> Completar), and the queue runs in
 * order, so when slot r comes round again everything up to its previous number is done.
 *
 * Ranges the guest rewrites every frame would cost a write fault per frame on the game's thread and never be reused:
 * after kSuciasParaVolatil stale finds a range stops being cached.
 *
 * Ring thread only, except EscrituraVigilada (guest threads, with the memory lock held: atomics only).
 *
 * A second instance holds FH2's vertex fetch buffers (fh2_nativo_cache_buscas): its buffer also has a transient area
 * per work slot for the copies it does not keep, because the shaders read every fetch through one binding.
 */
class CacheVerticesEntreFotogramas {
 public:
  // 4 MB segments: 16 (64 MB) by default, up to 48 (192 MB) with Configurar. On the S23 Ultra in daylight 64 MB were
  // not enough: every segment held copies used in the last submission, and 135,000-577,000 reservations per session
  // fell back to the upload buffer.
  static constexpr uint32_t kMaxSegmentos = 48;
  static constexpr uint64_t kTamanoSegmento = uint64_t(4) << 20;
  static constexpr uint32_t kMinBytes = 4096;                          // smaller ranges share pages with other data
  static constexpr uint32_t kMaxBytes = uint32_t(kTamanoSegmento / 4);
  static constexpr uint32_t kSuciasParaVolatil = 3;
  static constexpr uint32_t kMaxRanuras = 8;

  CacheVerticesEntreFotogramas() : ticks_pagina_(new std::atomic<uint32_t>[kPaginas]) {
    for (uint32_t i = 0; i < kPaginas; ++i) ticks_pagina_[i].store(0, std::memory_order_relaxed);
  }

  // Before the buffer is created: how many segments it has.
  void Configurar(uint32_t segmentos) { segmentos_ = std::clamp<uint32_t>(segmentos, 2, kMaxSegmentos); }
  uint64_t Tamano() const { return uint64_t(segmentos_) * kTamanoSegmento; }

  // Each work slot used (UsarRanura): the submission number of what is recorded now, and what the GPU has finished.
  void NuevaRanura(uint32_t ranura) {
    ranura %= kMaxRanuras;
    completado_ = std::max(completado_, serie_de_ranura_[ranura]);
    serie_de_ranura_[ranura] = ++serie_;
  }

  // Guest thread, memory lock held: stamps the written pages.
  static std::pair<uint32_t, uint32_t> EscrituraVigilada(void* contexto, uint32_t inicio, uint32_t longitud, bool) {
    auto* self = static_cast<CacheVerticesEntreFotogramas*>(contexto);
    const uint32_t tick = self->tick_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint64_t fin = std::min<uint64_t>(uint64_t(inicio) + longitud, kMemoriaFisica);
    for (uint64_t p = inicio >> kBitsPagina; (p << kBitsPagina) < fin; ++p) {
      self->ticks_pagina_[p].store(tick, std::memory_order_release);
    }
    self->avisos_.fetch_add(1, std::memory_order_relaxed);
    // Exactly the stamped range. The heap unwatches the intersection of what every callback returns, padded up
    // to a 4 MB block (PhysicalHeap::TriggerCallbacks, kMaxUnwatchExcess): returning everything let one write
    // drop the watch of every other cached page in its block without stamping them, and later writes there
    // went unseen (stale copies served).
    return {inicio, longitud};
  }

  // Whether the range can go through the cache at all.
  bool Admite(uint64_t direccion, uint32_t bytes) const {
    return bytes >= kMinBytes && bytes <= kMaxBytes && direccion + bytes <= kMemoriaFisica &&
           !volatiles_.count(Clave(direccion, bytes, 0) >> 2);
  }

  // A clean copy of the range made in an earlier submission or this one: its offset in the buffer. 'huella' != 0
  // compares contents too (fh2_nativo_cache_vertices_comprobar); a mismatch counts and misses.
  bool Buscar(uint64_t direccion, uint32_t bytes, uint32_t orden, uint64_t& offset, uint64_t huella) {
    ++consultas_;
    const auto it = entradas_.find(Clave(direccion, bytes, orden));
    if (it == entradas_.end()) {
      return false;
    }
    Entrada& e = it->second;
    const uint64_t ultima = (direccion + bytes - 1) >> kBitsPagina;
    for (uint64_t p = direccion >> kBitsPagina; p <= ultima; ++p) {
      if (int32_t(ticks_pagina_[p].load(std::memory_order_acquire) - e.tick) > 0) {
        ++sucias_;
        if (++e.sucias >= kSuciasParaVolatil) {
          volatiles_.insert(Clave(direccion, bytes, 0) >> 2);
          ++volatiles_n_;
        }
        return false;
      }
    }
    if (huella != 0 && huella != e.huella) {
      ++discrepancias_;
      return false;
    }
    e.sucias = 0;
    ultimo_uso_[e.segmento] = serie_;
    ++aciertos_;
    bytes_ahorrados_ += bytes;
    offset = e.offset;
    return true;
  }

  // Room for a new copy, at a multiple of 'paso' (the vertex stride on the base-zero path). The returned 'tick'
  // must be taken before the range's write watch is enabled.
  bool Reservar(uint32_t bytes, uint32_t paso, uint64_t& offset, uint32_t& tick) {
    paso = std::max<uint32_t>(paso, 4);
    for (int intento = 0; intento < 2; ++intento) {
      if (segmento_actual_ < segmentos_) {
        const uint64_t base = uint64_t(segmento_actual_) * kTamanoSegmento;
        const uint64_t inicio = (base + cursor_ + paso - 1) / paso * paso;
        if (inicio + bytes <= base + kTamanoSegmento) {
          cursor_ = inicio + bytes - base;
          offset = inicio;
          tick = tick_.load(std::memory_order_acquire);
          ultimo_uso_[segmento_actual_] = serie_;
          return true;
        }
      }
      if (!SiguienteSegmento()) {
        ++llenos_;
        return false;
      }
    }
    return false;
  }

  // Records the copy just queued at 'offset'. Replaces a stale copy of the same range (its old place is dropped
  // with its segment).
  void Anotar(uint64_t direccion, uint32_t bytes, uint32_t orden, uint64_t offset, uint32_t tick, uint64_t huella) {
    const uint64_t clave = Clave(direccion, bytes, orden);
    Entrada& e = entradas_[clave];
    const uint32_t sucias = e.sucias;
    e = Entrada{offset, huella, tick, sucias, uint8_t(offset / kTamanoSegmento)};
    claves_segmento_[e.segmento].push_back(clave);
    ++copias_;
    bytes_copiados_ += bytes;
  }

  uint64_t consultas() const { return consultas_; }
  uint64_t aciertos() const { return aciertos_; }
  uint64_t bytes_ahorrados() const { return bytes_ahorrados_; }
  uint64_t copias() const { return copias_; }
  uint64_t bytes_copiados() const { return bytes_copiados_; }
  uint64_t sucias() const { return sucias_; }
  uint64_t discrepancias() const { return discrepancias_; }
  uint64_t llenos() const { return llenos_; }
  uint64_t reciclados() const { return reciclados_; }
  uint64_t volatiles() const { return volatiles_n_; }
  uint64_t avisos() const { return avisos_.load(std::memory_order_relaxed); }
  size_t entradas() const { return entradas_.size(); }

 private:
  static constexpr uint64_t kMemoriaFisica = uint64_t(512) << 20;
  static constexpr uint32_t kBitsPagina = 12;
  static constexpr uint32_t kPaginas = uint32_t(kMemoriaFisica >> kBitsPagina);

  struct Entrada {
    uint64_t offset = 0;
    uint64_t huella = 0;
    uint32_t tick = 0;    // tick_ before the watch of its copy was enabled
    uint32_t sucias = 0;  // stale finds in a row
    uint8_t segmento = 0;
  };

  // Address (29 bits), bytes (up to kMaxBytes, 20 bits) and byte order (2 bits).
  static uint64_t Clave(uint64_t direccion, uint32_t bytes, uint32_t orden) {
    return (direccion << 22) ^ (uint64_t(bytes) << 2) ^ (uint64_t(orden) & 3);
  }

  // The least recently used segment the GPU has finished with; its copies are dropped.
  bool SiguienteSegmento() {
    uint32_t elegido = segmentos_;
    for (uint32_t s = 0; s < segmentos_; ++s) {
      if (s != segmento_actual_ && ultimo_uso_[s] <= completado_ &&
          (elegido == segmentos_ || ultimo_uso_[s] < ultimo_uso_[elegido])) {
        elegido = s;
      }
    }
    if (elegido == segmentos_) {
      return false;
    }
    for (uint64_t clave : claves_segmento_[elegido]) {
      const auto it = entradas_.find(clave);
      if (it != entradas_.end() && it->second.segmento == elegido) {
        entradas_.erase(it);
      }
    }
    claves_segmento_[elegido].clear();
    segmento_actual_ = elegido;
    cursor_ = 0;
    ultimo_uso_[elegido] = serie_;
    ++reciclados_;
    return true;
  }

  std::unique_ptr<std::atomic<uint32_t>[]> ticks_pagina_;
  std::atomic<uint32_t> tick_{0};
  std::atomic<uint64_t> avisos_{0};
  std::unordered_map<uint64_t, Entrada> entradas_;
  std::unordered_set<uint64_t> volatiles_;
  std::array<std::vector<uint64_t>, kMaxSegmentos> claves_segmento_{};
  std::array<uint64_t, kMaxSegmentos> ultimo_uso_{};  // submission number that last used each segment
  std::array<uint64_t, kMaxRanuras> serie_de_ranura_{};
  uint64_t serie_ = 0, completado_ = 0;
  uint32_t segmentos_ = 16;
  uint32_t segmento_actual_ = 0;
  uint64_t cursor_ = 0;

  uint64_t consultas_ = 0, aciertos_ = 0, bytes_ahorrados_ = 0, copias_ = 0, bytes_copiados_ = 0, sucias_ = 0;
  uint64_t discrepancias_ = 0, llenos_ = 0, reciclados_ = 0, volatiles_n_ = 0;
};

/*
 * The same across-frames mechanism for indices (fh2_nativo_cache_indices).
 *
 * The ring converts every draw's guest indices (byte swap, IndicesDe16 with NEON) and copies the converted
 * result to the frame's upload buffer, even though static meshes repeat the same guest range with the same
 * content every frame. This keeps the already converted result in a persistent buffer: a hit skips both the
 * conversion and the copy. The stored copy also carries the vertex min/max the conversion measured, which
 * Dibujar uses after it (upload clipping, base-zero path, DrawIndexed's vertexOffset).
 *
 * Only the DMA path qualifies: rectangle lists and quads generate or rewrite their indices (they have no
 * guest address) and never enter. Everything that changes the converted result is in the key: physical
 * address, count, bytes per index (16/32 bits), byte order, the restart flag and value, and the added offset.
 */
class CacheIndicesEntreFotogramas {
 public:
  // 4 MB segments: 4 (16 MB) by default, up to 8 (32 MB) with Configurar. Index traffic is about a tenth of
  // the vertex one (~1.1 GB vs ~12.5 GB per 10 s on the S23 Ultra), so this stays well below the vertex cache
  // (64/192 MB), respecting phones with 4 GB of RAM.
  static constexpr uint32_t kMaxSegmentos = 8;
  static constexpr uint64_t kTamanoSegmento = uint64_t(4) << 20;
  static constexpr uint32_t kMinBytes = 4096;  // smaller ranges share pages with other data
  static constexpr uint32_t kMaxBytes = uint32_t(kTamanoSegmento / 4);
  static constexpr uint32_t kSuciasParaVolatil = 3;
  static constexpr uint32_t kMaxRanuras = 8;

  struct Clave {
    uint64_t base = 0;  // physical address of the first guest index byte
    uint32_t cuenta = 0;
    uint32_t indice_reinicio = 0;
    uint32_t desplazamiento = 0;
    uint8_t bytes_por_indice = 2;  // 2 or 4
    uint8_t orden = 0;
    uint8_t reinicio = 0;
    bool operator==(const Clave& o) const {
      return base == o.base && cuenta == o.cuenta && indice_reinicio == o.indice_reinicio &&
             desplazamiento == o.desplazamiento && bytes_por_indice == o.bytes_por_indice &&
             orden == o.orden && reinicio == o.reinicio;
    }
  };
  struct ClaveHash {
    // FNV-1a over the raw fields, then the same multiply-and-shift mix as DedupeVertices.
    size_t operator()(const Clave& c) const {
      uint64_t x = 1469598103934665603ull;
      auto Mezclar = [&x](uint64_t v) {
        x ^= v;
        x *= 1099511628211ull;
      };
      Mezclar(c.base);
      Mezclar((uint64_t(c.cuenta) << 32) | c.indice_reinicio);
      Mezclar((uint64_t(c.desplazamiento) << 32) |
              (uint64_t(c.bytes_por_indice) << 16) | (uint64_t(c.orden) << 8) | c.reinicio);
      x ^= x >> 30;
      x *= 0xBF58476D1CE4E5B9ull;
      x ^= x >> 27;
      return size_t(x);
    }
  };

  CacheIndicesEntreFotogramas() : ticks_pagina_(new std::atomic<uint32_t>[kPaginas]) {
    for (uint32_t i = 0; i < kPaginas; ++i) ticks_pagina_[i].store(0, std::memory_order_relaxed);
  }

  // Before the buffer is created: how many segments it has.
  void Configurar(uint32_t segmentos) { segmentos_ = std::clamp<uint32_t>(segmentos, 2, kMaxSegmentos); }
  uint64_t Tamano() const { return uint64_t(segmentos_) * kTamanoSegmento; }

  // Each work slot used (UsarRanura): the submission number of what is recorded now, and what the GPU has finished.
  void NuevaRanura(uint32_t ranura) {
    ranura %= kMaxRanuras;
    completado_ = std::max(completado_, serie_de_ranura_[ranura]);
    serie_de_ranura_[ranura] = ++serie_;
  }

  // Guest thread, memory lock held: stamps the written pages.
  static std::pair<uint32_t, uint32_t> EscrituraVigilada(void* contexto, uint32_t inicio, uint32_t longitud, bool) {
    auto* self = static_cast<CacheIndicesEntreFotogramas*>(contexto);
    const uint32_t tick = self->tick_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint64_t fin = std::min<uint64_t>(uint64_t(inicio) + longitud, kMemoriaFisica);
    for (uint64_t p = inicio >> kBitsPagina; (p << kBitsPagina) < fin; ++p) {
      self->ticks_pagina_[p].store(tick, std::memory_order_release);
    }
    self->avisos_.fetch_add(1, std::memory_order_relaxed);
    return {inicio, longitud};  // only the stamped range (see CacheVerticesEntreFotogramas::EscrituraVigilada)
  }

  // Whether the range can go through the cache at all.
  bool Admite(const Clave& clave) const {
    const uint64_t bytes = uint64_t(clave.cuenta) * clave.bytes_por_indice;
    return bytes >= kMinBytes && bytes <= kMaxBytes && clave.base + bytes <= kMemoriaFisica &&
           volatiles_.find(clave) == volatiles_.end();
  }

  // A clean converted copy from an earlier submission or this one. On a hit, 'offset' holds its place in the
  // buffer and 'vmin'/'vmax'/'de16' describe it, so the conversion (and its min/max pass) is skipped.
  // 'huella' != 0 compares contents too (fh2_nativo_cache_indices_comprobar); a mismatch counts and misses.
  // 'huella_copia' (check mode only) returns the fingerprint of the converted copy as it was stored, so the
  // caller can also prove the persistent buffer still holds it.
  bool Buscar(const Clave& clave, uint64_t& offset, uint32_t& vmin, uint32_t& vmax,
              bool& de16, uint64_t huella, uint64_t* huella_copia = nullptr) {
    ++consultas_;
    const auto it = entradas_.find(clave);
    if (it == entradas_.end()) {
      return false;
    }
    Entrada& e = it->second;
    const uint64_t bytes = uint64_t(clave.cuenta) * clave.bytes_por_indice;
    const uint64_t ultima = (clave.base + bytes - 1) >> kBitsPagina;
    for (uint64_t p = clave.base >> kBitsPagina; p <= ultima; ++p) {
      if (int32_t(ticks_pagina_[p].load(std::memory_order_acquire) - e.tick) > 0) {
        ++sucias_;
        if (++e.sucias >= kSuciasParaVolatil) {
          volatiles_.insert(clave);
          ++volatiles_n_;
        }
        return false;
      }
    }
    if (huella != 0 && huella != e.huella) {
      ++discrepancias_;
      return false;
    }
    e.sucias = 0;
    ultimo_uso_[e.segmento] = serie_;
    ++aciertos_;
    bytes_ahorrados_ += uint32_t(bytes);
    offset = e.offset;
    vmin = e.vmin;
    vmax = e.vmax;
    de16 = e.de16;
    if (huella_copia) {
      *huella_copia = e.huella_copia;
    }
    return true;
  }

  // Room for a new converted copy, at a multiple of 4. The returned 'tick' must be taken before the range's
  // write watch is enabled.
  bool Reservar(uint32_t bytes, uint64_t& offset, uint32_t& tick) {
    constexpr uint32_t paso = 4;
    for (int intento = 0; intento < 2; ++intento) {
      if (segmento_actual_ < segmentos_) {
        const uint64_t base = uint64_t(segmento_actual_) * kTamanoSegmento;
        const uint64_t inicio = (base + cursor_ + paso - 1) / paso * paso;
        if (inicio + bytes <= base + kTamanoSegmento) {
          cursor_ = inicio + bytes - base;
          offset = inicio;
          tick = tick_.load(std::memory_order_acquire);
          ultimo_uso_[segmento_actual_] = serie_;
          return true;
        }
      }
      if (!SiguienteSegmento()) {
        ++llenos_;
        return false;
      }
    }
    return false;
  }

  // Records the converted copy just queued at 'offset'. Replaces a stale copy of the same key (its old place
  // is dropped with its segment).
  void Anotar(const Clave& clave, uint64_t offset, uint32_t tick, uint64_t huella, uint32_t vmin, uint32_t vmax,
              bool de16, uint64_t huella_copia = 0) {
    Entrada& e = entradas_[clave];
    const uint32_t sucias = e.sucias;
    e = Entrada{offset, huella, tick, sucias, uint8_t(offset / kTamanoSegmento), vmin, vmax, de16, huella_copia};
    claves_segmento_[e.segmento].push_back(clave);
    ++copias_;
    bytes_copiados_ += uint64_t(clave.cuenta) * clave.bytes_por_indice;
  }

  uint64_t consultas() const { return consultas_; }
  uint64_t aciertos() const { return aciertos_; }
  uint64_t bytes_ahorrados() const { return bytes_ahorrados_; }
  uint64_t copias() const { return copias_; }
  uint64_t bytes_copiados() const { return bytes_copiados_; }
  uint64_t sucias() const { return sucias_; }
  uint64_t discrepancias() const { return discrepancias_; }
  uint64_t llenos() const { return llenos_; }
  uint64_t reciclados() const { return reciclados_; }
  uint64_t volatiles() const { return volatiles_n_; }
  uint64_t avisos() const { return avisos_.load(std::memory_order_relaxed); }
  size_t entradas() const { return entradas_.size(); }
  // Check mode: a hit whose stored copy no longer matches what was written there (overwritten in the buffer).
  void Pisada() { ++pisadas_; }
  uint64_t pisadas() const { return pisadas_; }

 private:
  static constexpr uint64_t kMemoriaFisica = uint64_t(512) << 20;
  static constexpr uint32_t kBitsPagina = 12;
  static constexpr uint32_t kPaginas = uint32_t(kMemoriaFisica >> kBitsPagina);

  struct Entrada {
    uint64_t offset = 0;
    uint64_t huella = 0;
    uint32_t tick = 0;    // tick_ before the watch of its copy was enabled
    uint32_t sucias = 0;  // stale finds in a row
    uint8_t segmento = 0;
    uint32_t vmin = 0;
    uint32_t vmax = 0;
    bool de16 = true;
    uint64_t huella_copia = 0;  // check mode: XXH3 of the converted copy when it was stored
  };

  // The least recently used segment the GPU has finished with; its copies are dropped.
  bool SiguienteSegmento() {
    uint32_t elegido = segmentos_;
    for (uint32_t s = 0; s < segmentos_; ++s) {
      if (s != segmento_actual_ && ultimo_uso_[s] <= completado_ &&
          (elegido == segmentos_ || ultimo_uso_[s] < ultimo_uso_[elegido])) {
        elegido = s;
      }
    }
    if (elegido == segmentos_) {
      return false;
    }
    for (const Clave& clave : claves_segmento_[elegido]) {
      const auto it = entradas_.find(clave);
      if (it != entradas_.end() && it->second.segmento == elegido) {
        entradas_.erase(it);
      }
    }
    claves_segmento_[elegido].clear();
    segmento_actual_ = elegido;
    cursor_ = 0;
    ultimo_uso_[elegido] = serie_;
    ++reciclados_;
    return true;
  }

  std::unique_ptr<std::atomic<uint32_t>[]> ticks_pagina_;
  std::atomic<uint32_t> tick_{0};
  std::atomic<uint64_t> avisos_{0};
  std::unordered_map<Clave, Entrada, ClaveHash> entradas_;
  std::unordered_set<Clave, ClaveHash> volatiles_;
  std::array<std::vector<Clave>, kMaxSegmentos> claves_segmento_{};
  std::array<uint64_t, kMaxSegmentos> ultimo_uso_{};  // submission number that last used each segment
  std::array<uint64_t, kMaxRanuras> serie_de_ranura_{};
  uint64_t serie_ = 0, completado_ = 0;
  uint32_t segmentos_ = 4;
  uint32_t segmento_actual_ = 0;
  uint64_t cursor_ = 0;

  uint64_t consultas_ = 0, aciertos_ = 0, bytes_ahorrados_ = 0, copias_ = 0, bytes_copiados_ = 0, sucias_ = 0;
  uint64_t discrepancias_ = 0, llenos_ = 0, reciclados_ = 0, volatiles_n_ = 0, pisadas_ = 0;
};

}  // namespace nfsmw::nativo
