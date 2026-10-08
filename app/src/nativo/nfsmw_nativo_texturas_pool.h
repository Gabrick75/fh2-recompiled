// nfsmw - native renderer, C3: sub-allocation pool for textures.
//
// Why it exists (measured, not estimated)
//
//   Without the pool, every texture gets its own dedicated allocation: CrearTextura ->
//   rex::ui::vulkan::util::CreateDedicatedAllocationImage -> vkAllocateMemory with
//   VkMemoryDedicatedAllocateInfo. On Horizon that is not a cheap call.
//
//   What every texture pays, following the chain in the Mesa tree:
//     1. nvk_AllocateMemory (nvk_device_memory.c:133); in the __SWITCH__ branch
//        (:178-193) it sets alignment, tile_mode and pte_kind.
//     2. nvkmd_switch_dev_alloc_mem_impl (nvkmd/switch/nvkmd_switch_dev.c:750-838):
//        memory_create -> memory_map -> alloc_va (VA #1) -> va_bind_mem (mapping #1).
//     3. nouveau_horizon_memory_create (horizon/nouveau_horizon_memory.c:468-663):
//        memalign + nvMapCreate (:572) and, always, memset(size) + armDCacheClean(size)
//        (:612-620), even though Vulkan does not ask for ZERO_INITIALIZE.
//     4. nvk_image_plane_bind (nvk_image.c:1697-1707): since our textures are
//        SAMPLED|TRANSFER_DST, nvk_image_can_compress returns false (nvk_image.c:843-846),
//        so the dedicated shortcut is not taken: a VA #2 is reserved with the block-linear
//        pte_kind and a mapping #2 is made.
//
//   In short: the dedicated allocation costs twice the VAs and mappings and gives nothing back.
//   Its only benefit is compression, and our textures are never compressible.
//
// The measurement (console, entering a new zone)
//
//   From rex_perfil.log:
//     "NvMap nuevos 17.6 con cache (2.8 MB) y 0.3 sin cache (0.0 MB), 7.8 ms |
//      direcciones de GPU 35.4 (4.0 ms), mapeos 35.4 (0.0 MB, 19.2 ms)"
//   -> 31.0 ms/s of CPU just allocating and mapping. And 35.4/17.9 = 1.98: two VAs and two
//      mappings per NvMap, exactly what the code above predicts.
//
//   Medians per call, 80 intervals with >=5 allocations/s across several logs
//   (the counters come from the --wrap wrappers in tools/switch/sdk/switch_perf.cpp:789-810):
//     nvMapCreate                422 us
//     nvAddressSpaceAllocFixed   127 us
//     MapBufferEx                626 us
//     One texture (1 + 2 + 2)   ~1.9 ms
//
//   Real peaks: 159 ms/s (16 % of a core) and 298.7 ms/s (30 % of a core).
//
//   The driver's BO cache does not save this: in a long measurement it hits 88 % and
//   everything is still paid, because recycling saves the nvMapCreate but not the VA or the
//   mapping, which are the expensive two thirds.
//
// What this pool saves
//
//   Per texture only the plane's VA and its partial mapping remain: 1.9 ms -> ~0.75 ms (-60 %).
//     Bad stretch:                  31.0 ms/s -> 18.8 ms/s   (-12.2 ms/s)
//     Average frame (~27 FPS):       1.15 ms  ->  0.70 ms    (-0.45 ms)
//     Burst of 15 textures:         28.5 ms   -> 11.3 ms     (-17.2 ms)
//   It is a variance fix, not an average one: allocation explains 9 % of the average gap
//   but 17 of the 44 ms of the worst frame, which is what is noticeable when turning.
//
//   As a bonus: live NvMap handles drop from ~3,600 to ~12, and with them the ceiling of
//   the area shared with nvdrv goes away (error 0x235C, which showed up as a black screen;
//   see nfsmw_entorno_mesa.cpp:30-44).
//
// Why not a single pixel changes
//
//   Binding at offset != 0 into non-dedicated memory is supported and keeps the block-linear
//   layout: NVK reserves a VA of its own for the plane with the right pte_kind and maps the
//   sub-range (nvk_image.c:1700-1710 -> nouveau_horizon_vm.c:374-394).
//   The size vkGetImageMemoryRequirements returns is already rounded to 64 KiB precisely for
//   this (nvk_image.c:1422-1428), and requiresDedicatedAllocation is never true on Switch
//   (nvk_image.c:1444-1460). The slabs are allocated without VkMemoryDedicatedAllocateInfo,
//   so their layout.valid is false and they accept any pte_kind (nvkmd_switch_dev.c:787-795,
//   nouveau_horizon_memory.c:1056-1073).
//   No compression is lost because there was none (nvk_image.c:843-846).
//
// Safety invariant (mandatory, read before touching eviction)
//
//   Without the pool, if the GPU read a texture that was just destroyed it would read memory
//   that was freed but still mapped: harmless garbage. With the pool it would read the pixels
//   of another texture, because the physical block is reused immediately.
//
//   That is only safe while both of these conditions hold. The current code meets them, and
//   they are mandatory:
//     a) Normal eviction only releases textures unused for 120 frames
//        (kFotogramasSinUsoParaSoltar, nfsmw_nativo_dibujos.cpp:2695-2705). No pending work
//        can reference them.
//     b) Emergency eviction (SoltarTexturasPorFaltaDeMemoria, :2835-2850) calls
//        EsperarGpuDelTodo() first.
//   If that threshold of 120 is ever lowered or that wait removed, this pool is no longer
//   safe and releasing a block must be delayed by a few frames.
//
// Single thread
//
//   All of this lives on the ring thread (PrepararTextura in C3 and render targets in C2),
//   so there is no lock. If a texture is ever created from another thread, a lock must be
//   added here first.

#pragma once

#include <rex/ui/vulkan/device.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nfsmw::nativo {

// Opaque identifier of a pool block. This value means "not from the pool" and is the
// default value ImagenNativa::pool_bloque must hold.
inline constexpr uint32_t kBloquePoolInvalido = UINT32_MAX;

// Pool granularity. Horizon binds with 64 KiB alignment
// (NOUVEAU_HORIZON_BIND_ALIGN_B, horizon/nouveau_horizon_private.h:21) and NVK already
// rounds image sizes to that value, so no memory is lost: it is the same rounding the
// dedicated allocation already pays.
inline constexpr uint64_t kUnidadPoolBytes = 64ull * 1024ull;

// For the AnotarMemoriaDeLaGpu report.
struct EstadoPoolTexturas {
  bool activo = false;
  uint32_t tipo_memoria = UINT32_MAX;
  uint32_t slabs = 0;
  uint64_t bytes_reservados = 0;  // sum of the slab sizes
  uint64_t bytes_en_uso = 0;      // space used by live textures
  uint64_t bytes_libres = 0;
  uint64_t bytes_mayor_hueco = 0;  // largest contiguous free range: measures fragmentation
  uint64_t texturas_vivas = 0;
  uint64_t texturas_colocadas = 0;   // running total that went into the pool
  uint64_t texturas_dedicadas = 0;   // running total that fell back to the dedicated path
  uint64_t huecos_fallados = 0;      // times it did not fit in any slab
  uint64_t slabs_en_caliente = 0;    // slabs created after prewarming
  uint64_t slabs_fallados = 0;       // times vkAllocateMemory refused a slab
};

/*
 * GPU memory sub-allocator for the native renderer's textures.
 *
 * Intended use, all from the ring thread:
 *   Iniciar(dispositivo, mb_cache_max)      once, in Inicializar(), before drawing
 *   PorFotograma(fotograma)                 once per frame, to grow with headroom
 *   Reservar(requisitos, mem, offset, blq)  in CrearTextura, before vkBindImageMemory
 *   AnotarDedicada()                        when a texture falls back to the dedicated path
 *   Liberar(bloque)                         in DestruirImagen
 *   Terminar()                              in the destructor, after the images are destroyed
 */
class PoolTexturas {
 public:
  PoolTexturas() = default;
  ~PoolTexturas();

  PoolTexturas(const PoolTexturas&) = delete;
  PoolTexturas& operator=(const PoolTexturas&) = delete;

  /*
   * Prepares the pool and prewarms the slabs for the steady state.
   *
   * `mb_cache_max` is nfsmw_nativo_texturas_mb_max: it gives the initial MB (half of it,
   * between 64 and 256) and the cap (the cache limit plus one slab of headroom).
   *
   * Returns false if the cvar is off, if there is no valid memory type or if not even one
   * slab could be created. In that case Reservar() always fails and everything keeps using
   * the usual dedicated path, exactly the behavior without the pool.
   */
  bool Iniciar(const rex::ui::vulkan::VulkanDevice* dispositivo, int32_t mb_cache_max);

  // Frees every slab. The images must already be destroyed.
  void Terminar();

  bool Activo() const { return activo_; }

  /*
   * Finds room for an already created image.
   *
   * `requisitos` is what vkGetImageMemoryRequirements returns for that VkImage.
   * requisitos.alignment is honored exactly, it is not assumed to be 64 KiB: a 3D texture
   * can ask for up to 512 KB because of the tile mode's z_log2 (nil/image.rs:430).
   *
   * If it returns true, the caller must call vkBindImageMemory(imagen, memoria_out,
   * offset_out) and store bloque_out in ImagenNativa::pool_bloque.
   * If it returns false, nothing has been touched: the caller uses the dedicated path.
   */
  bool Reservar(const VkMemoryRequirements& requisitos, VkDeviceMemory& memoria_out,
                VkDeviceSize& offset_out, uint32_t& bloque_out);

  // Returns the block to the pool. Accepts kBloquePoolInvalido and does nothing with it.
  void Liberar(uint32_t bloque);

  /*
   * Growth with headroom. Called once per frame.
   *
   * Creates a new slab when less than kHolguraBytes is free, never at the moment it is
   * needed: nouveau_horizon_memory_create does a memset + dcache clean of the whole slab
   * (:612-620), and 32 MB of that in the middle of a race would be a 10-15 ms stutter, the
   * very problem this pool solves. It also creates at most one every kFotogramasEntreSlabs.
   */
  void PorFotograma(uint64_t fotograma);

  // A texture did not fit (or the pool is off) and took the dedicated path.
  void AnotarDedicada() { ++texturas_dedicadas_; }

  EstadoPoolTexturas Estado() const;

  // Line for the C3 report of AnotarMemoriaDeLaGpu. No trailing newline.
  std::string Resumen() const;

 private:
  struct Slab {
    VkDeviceMemory memoria = VK_NULL_HANDLE;
    uint32_t unidades = 0;
    uint32_t unidades_en_uso = 0;
    std::vector<uint64_t> ocupadas;  // bitmap, one unit per bit
    std::vector<uint32_t> largo;     // units reserved from each start (0 = not a start)
  };

  // 8 MB of headroom: below that a new slab is requested.
  static constexpr uint64_t kHolguraBytes = 8ull << 20;
  // Never two slabs in a row: gives eviction time to free memory before growing further.
  static constexpr uint64_t kFotogramasEntreSlabs = 120;
  // The slab index goes in the top 8 bits of the block identifier.
  static constexpr uint32_t kMaxSlabs = 255;

  bool CrearSlab(bool en_caliente);
  bool ElegirTipoDeMemoria(uint32_t& tipo_out, uint64_t& alineacion_vista_out) const;

  const rex::ui::vulkan::VulkanDevice* dispositivo_ = nullptr;
  VkDevice device_ = VK_NULL_HANDLE;
  bool activo_ = false;
  uint32_t tipo_memoria_ = UINT32_MAX;
  uint64_t slab_bytes_ = 0;
  uint32_t slab_unidades_ = 0;
  uint32_t slabs_tope_ = 0;
  uint64_t ultimo_crecimiento_ = 0;
  std::vector<Slab> slabs_;

  uint64_t texturas_vivas_ = 0;
  uint64_t texturas_colocadas_ = 0;
  uint64_t texturas_dedicadas_ = 0;
  uint64_t huecos_fallados_ = 0;
  uint64_t slabs_en_caliente_ = 0;
  uint64_t slabs_fallados_ = 0;
};

}  // namespace nfsmw::nativo
