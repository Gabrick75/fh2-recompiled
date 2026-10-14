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

#include <array>
#include <cstdint>
#include <vector>
#include <atomic>
#include <cstring>

#include <vulkan/vulkan.h>
#include <rex/logging.h>
#include <xxhash.h>

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

// Persistent vertex cache across frames with write-watching.
// Keyed by (physical address, bytes, byte order). Validity comes from the write-watch callback:
// if the guest has not written to the pages since the copy, the range is reused without copying.
// Never writes in place: if a range becomes dirty, a new spot is allocated and the old one is
// freed after the work slot that last used it has passed its fence. A bump allocator with
// deferred freeing per work slot handles this.
//
// The cache uses a persistent upload buffer that is not reset in UsarRanura. It is separate
// from the per-frame upload buffer to avoid rebinding vertex buffers (which has a cost).
// Small ranges (< 4 KB) are not cached because they share pages with other data and would
// be marked dirty constantly (the FH2 lookups already copy them every time).
class CacheVerticesPersistente {
 public:
  // Called once at startup to initialize with a pre-created persistent buffer.
  void Inicializar(VkBuffer buffer, VkDeviceMemory memoria, uint8_t* datos, VkDeviceAddress direccion,
                   rex::memory::Memory* memoria_contexto);

  // Called when a new work slot is activated (UsarRanura). The frame epoch is bumped so
  // that entries from previous slots are not reused until their slot's fence has passed.
  void NuevaRanura(uint64_t epoca_subida, uint32_t ranura_actual);

  // Called by the write-watch callback when the guest writes to a watched range.
  // Only atomics here: called on the guest writing thread with the memory lock held.
  static std::pair<uint32_t, uint32_t> EscrituraVigilada(void* contexto, uint32_t inicio, uint32_t longitud, bool exacto);

  // Tries to find a cached copy of the vertex range. If found and clean, returns true
  // and sets 'offset' to the location in the persistent buffer. If paranoid mode is
  // enabled (huella != 0), also verifies the content fingerprint.
  bool Buscar(uint64_t direccion_fisica, uint32_t bytes, uint32_t orden, uint64_t& offset,
              uint64_t huella = 0);

  // Records a new copy in the persistent buffer. 'offset' is the location in the
  // persistent buffer where the data was copied. Returns the watch handle for the range.
  void* Anotar(uint64_t direccion_fisica, uint32_t bytes, uint32_t orden, uint64_t offset,
               uint64_t huella = 0);

  // Allocates space in the persistent buffer for a new vertex range.
  // Returns true and sets 'offset' on success, false if buffer is full.
  bool Alocar(uint32_t bytes, uint32_t alineacao, uint64_t& offset);

  // Called when a work slot's fence has signaled. Frees all entries that were
  // deferred for that slot.
  void LiberarRanura(uint32_t ranura);

  // Returns the persistent buffer handle for binding.
  VkBuffer Buffer() const { return buffer_; }

  // Returns the mapped persistent buffer data pointer.
  uint8_t* Dados() const { return datos_; }

  // Computes XXH3 fingerprint of guest memory.
  static uint64_t CalcularHuella(const uint8_t* datos, uint32_t bytes);

  // Persistent buffer size: 64 MB (same as per-frame upload buffer).
  static constexpr VkDeviceSize kTamanoBuffer = VkDeviceSize(64) << 20;

  // Statistics for the C6 report.
  uint64_t consultas() const { return consultas_; }
  uint64_t aciertos() const { return aciertos_; }
  uint64_t bytes_ahorrados() const { return bytes_ahorrados_; }
  uint64_t copias() const { return copias_; }
  uint64_t memoria_ocupada() const { return memoria_ocupada_; }
  uint64_t liberaciones_adiadas() const { return liberaciones_adiadas_; }
  uint64_t discrepancias() const { return discrepancias_; }
  uint64_t rangos_pequenos() const { return rangos_pequenos_; }

 private:
  // Minimum size to cache: 4 KB (page size). Smaller ranges share pages and get dirty constantly.
  static constexpr uint32_t kMinBytesCache = 4 << 10;

  // Max entries in the hash table.
  static constexpr size_t kRanuras = 32768;

  // Work slots (matches kRanurasDeTrabajo = 3).
  static constexpr uint32_t kRanurasTrabajo = 3;

  struct Entrada {
    uint64_t clave = 0;           // (direccion_fisica << 2) | orden
    uint32_t bytes = 0;
    uint32_t orden = 0;
    VkDeviceSize offset = 0;      // offset in persistent buffer
    uint64_t huella = 0;          // XXH3 fingerprint (paranoid mode)
    uint64_t epoca_creacion = 0;  // epoca_subida_ when this entry was created
    uint32_t ranura_ultima = 0;   // work slot that last used this entry
    uint32_t sucio = 0;           // 1 = guest has written to this range (atomic access via pointer)
    void* vigilancia_handle = nullptr;  // handle from RegisterPhysicalMemoryInvalidationCallback
    uint64_t padding = 0;         // pad to 64 bytes
  };
  static_assert(sizeof(Entrada) == 64, "entry size");

  // Hash table entry (open addressing with linear probing).
  struct Casilla {
    uint64_t clave = 0;   // 0 = empty
    uint32_t indice = 0;  // index into entradas_
  };
  static_assert(sizeof(Casilla) == 16, "casilla size");

  static uint64_t Clave(uint64_t direccion, uint32_t orden) {
    return (direccion << 2) | (uint64_t(orden) & 0x3);
  }

  static size_t Indice(uint64_t clave) {
    uint64_t x = clave ^ 0x9E3779B97F4A7C15ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    return size_t(x) & (kRanuras - 1);
  }

  std::array<Casilla, kRanuras> tabla_{};
  std::vector<Entrada> entradas_;
  uint32_t entradas_n_ = 0;

  // Bump allocator for the persistent buffer.
  VkDeviceSize buffer_usado_ = 0;
  VkDevice dispositivo_ = VK_NULL_HANDLE;
  VkBuffer buffer_ = VK_NULL_HANDLE;
  VkDeviceMemory memoria_ = VK_NULL_HANDLE;
  uint8_t* datos_ = nullptr;
  VkDeviceAddress direccion_buffer_ = 0;

  // Per-work-slot deferred freeing: each slot has a list of entries to free when its fence passes.
  std::array<std::vector<uint32_t>, kRanurasTrabajo> liberar_despues_de_ranura_;

  uint64_t epoca_subida_actual_ = 0;
  uint32_t ranura_actual_ = 0;

  // Write-watch callback registration.
  void* vigilancia_global_ = nullptr;
  void* memoria_contexto_ = nullptr;
  void (*callback_escritura_)(void*, uint32_t, uint32_t, bool) = nullptr;

  // Statistics.
  uint64_t consultas_ = 0;
  uint64_t aciertos_ = 0;
  uint64_t bytes_ahorrados_ = 0;
  uint64_t copias_ = 0;
  uint64_t memoria_ocupada_ = 0;
  uint64_t liberaciones_adiadas_ = 0;
  uint64_t discrepancias_ = 0;
  uint64_t rangos_pequenos_ = 0;

  // Finds an entry in the hash table. Returns index into entradas_ or UINT32_MAX.
  uint32_t BuscarEnTabla(uint64_t clave, uint32_t bytes) const;

  // Inserts an entry into the hash table.
  void InsertarEnTabla(uint64_t clave, uint32_t bytes, uint32_t indice_entrada);
};

}  // namespace nfsmw::nativo
