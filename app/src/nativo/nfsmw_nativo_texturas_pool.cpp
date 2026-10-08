// nfsmw - native renderer, C3: sub-allocation pool for textures.
// The rationale, the measurements and the safety invariant are in nfsmw_nativo_texturas_pool.h.

#include "nfsmw_nativo_texturas_pool.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/util.h>

#include <algorithm>
#include <string>

REXCVAR_DEFINE_BOOL(nfsmw_nativo_texturas_pool, true, "NFSMW",
                    "Renderizador nativo (22/09): las texturas toman trozos de bloques grandes de memoria "
                    "en vez de pedir una reserva dedicada cada una. En Horizon cada reserva dedicada cuesta "
                    "~1,9 ms de CPU (un nvMapCreate, DOS reservas de direccion y DOS mapeos), y la imagen no "
                    "cambia. Apagarlo vuelve a la reserva por textura")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_INT32(nfsmw_nativo_texturas_pool_slab_mb, 32, "NFSMW",
                     "Renderizador nativo: MB de cada bloque grande del pool de texturas. Mas grande = menos "
                     "reservas al sistema, pero cada bloque nuevo cuesta un memset de ese tamano")
    .range(4, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace nfsmw::nativo {
namespace {

uint32_t AlinearArriba(uint32_t valor, uint32_t paso) {
  if (paso <= 1) {
    return valor;
  }
  return ((valor + paso - 1) / paso) * paso;
}

bool BitOcupado(const std::vector<uint64_t>& mapa, uint32_t i) {
  return ((mapa[i >> 6] >> (i & 63)) & 1ull) != 0ull;
}

// Returns true if [inicio, inicio+n) is entirely free. Otherwise it stores in `primera_ocupada`
// the first used unit it found, so the search can jump past it.
bool RangoLibre(const std::vector<uint64_t>& mapa, uint32_t inicio, uint32_t n,
                uint32_t& primera_ocupada) {
  for (uint32_t i = inicio; i < inicio + n; ++i) {
    if (BitOcupado(mapa, i)) {
      primera_ocupada = i;
      return false;
    }
  }
  return true;
}

void MarcarRango(std::vector<uint64_t>& mapa, uint32_t inicio, uint32_t n, bool ocupado) {
  for (uint32_t i = inicio; i < inicio + n; ++i) {
    if (ocupado) {
      mapa[i >> 6] |= 1ull << (i & 63);
    } else {
      mapa[i >> 6] &= ~(1ull << (i & 63));
    }
  }
}

/*
 * First fit, without a rover.
 *
 * With 32 MB blocks there are 512 units, so a full pass is at most a few hundred bit checks,
 * and jumping past the first used unit keeps it linear. At ten or twenty textures per second
 * this does not show up in measurements; a rover would only add a second pass and more places
 * to get it wrong.
 */
bool BuscarHueco(const std::vector<uint64_t>& mapa, uint32_t unidades_totales, uint32_t n,
                 uint32_t paso, uint32_t& inicio_out) {
  if (n == 0 || n > unidades_totales) {
    return false;
  }
  uint32_t inicio = 0;
  while (inicio + n <= unidades_totales) {
    uint32_t ocupada = 0;
    if (RangoLibre(mapa, inicio, n, ocupada)) {
      inicio_out = inicio;
      return true;
    }
    inicio = AlinearArriba(ocupada + 1, paso);
  }
  return false;
}

// The largest contiguous free range, in units. It is the fragmentation measure in the report.
uint32_t MayorHueco(const std::vector<uint64_t>& mapa, uint32_t unidades_totales) {
  uint32_t mejor = 0;
  uint32_t actual = 0;
  for (uint32_t i = 0; i < unidades_totales; ++i) {
    if (BitOcupado(mapa, i)) {
      actual = 0;
    } else {
      ++actual;
      mejor = std::max(mejor, actual);
    }
  }
  return mejor;
}

}  // namespace

PoolTexturas::~PoolTexturas() { Terminar(); }

bool PoolTexturas::ElegirTipoDeMemoria(uint32_t& tipo_out, uint64_t& alineacion_vista_out) const {
  /*
   * A sample image with the same usage and tiling as the real textures, only to ask the
   * driver which memory types it accepts and with what alignment. It is destroyed right
   * away; it does not allocate memory.
   */
  const auto& dfn = dispositivo_->functions();
  VkImageCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_R8G8B8A8_UNORM;
  info.extent = {256, 256, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage muestra = VK_NULL_HANDLE;
  if (dfn.vkCreateImage(device_, &info, nullptr, &muestra) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requisitos{};
  dfn.vkGetImageMemoryRequirements(device_, muestra, &requisitos);
  dfn.vkDestroyImage(device_, muestra, nullptr);

  // The same criterion CreateDedicatedAllocationImage uses for textures, so the slab lands
  // in the same memory type the dedicated allocation would.
  const uint32_t tipo = rex::ui::vulkan::util::ChooseMemoryType(
      dispositivo_->memory_types(), requisitos.memoryTypeBits,
      rex::ui::vulkan::util::MemoryPurpose::kDeviceLocal);
  if (tipo == UINT32_MAX) {
    return false;
  }
  tipo_out = tipo;
  alineacion_vista_out = requisitos.alignment;
  return true;
}

bool PoolTexturas::Iniciar(const rex::ui::vulkan::VulkanDevice* dispositivo, int32_t mb_cache_max) {
  if (!REXCVAR_GET(nfsmw_nativo_texturas_pool)) {
    REXLOG_INFO("[nativo] C3: pool de texturas (nfsmw_nativo_texturas_pool) = no; cada textura pedira su "
                "reserva dedicada, como antes del 22/09");
    return false;
  }
  if (dispositivo == nullptr) {
    return false;
  }
  dispositivo_ = dispositivo;
  device_ = dispositivo->device();

  uint64_t alineacion_vista = 0;
  if (!ElegirTipoDeMemoria(tipo_memoria_, alineacion_vista)) {
    REXLOG_WARN("[nativo] C3: pool de texturas: no hay un tipo de memoria local valido; se sigue con la "
                "reserva dedicada por textura");
    dispositivo_ = nullptr;
    device_ = VK_NULL_HANDLE;
    return false;
  }

  const int32_t slab_mb = std::max<int32_t>(REXCVAR_GET(nfsmw_nativo_texturas_pool_slab_mb), 4);
  slab_bytes_ = uint64_t(slab_mb) << 20;
  slab_unidades_ = uint32_t(slab_bytes_ / kUnidadPoolBytes);

  /*
   * How much to prewarm and how far to grow.
   *
   * At startup, half the cache limit (clamped between 64 and 256 MB): the cache currently
   * stays at about 159 MB of 384, so with that the pool almost never needs to grow during a
   * race. The cap is one slab above the cache limit to leave room for fragmentation.
   */
  const int32_t mb_max = mb_cache_max > 0 ? mb_cache_max : 384;
  const int32_t mb_inicial = std::clamp<int32_t>(mb_max / 2, 64, 256);
  const uint64_t bytes_tope = (uint64_t(mb_max) << 20) + slab_bytes_;
  slabs_tope_ = uint32_t(std::min<uint64_t>((bytes_tope + slab_bytes_ - 1) / slab_bytes_, kMaxSlabs));
  const uint32_t slabs_iniciales =
      uint32_t(std::min<uint64_t>(((uint64_t(mb_inicial) << 20) + slab_bytes_ - 1) / slab_bytes_, slabs_tope_));

  activo_ = true;  // CrearSlab lo necesita puesto
  for (uint32_t i = 0; i < slabs_iniciales; ++i) {
    if (!CrearSlab(false)) {
      break;
    }
  }
  if (slabs_.empty()) {
    REXLOG_WARN("[nativo] C3: pool de texturas: no se pudo crear ni un bloque de {} MB; se sigue con la "
                "reserva dedicada por textura",
                slab_mb);
    activo_ = false;
    dispositivo_ = nullptr;
    device_ = VK_NULL_HANDLE;
    return false;
  }

  REXLOG_INFO("[nativo] C3: pool de texturas ENCENDIDO: {} bloques de {} MB precalentados ({} MB), tope {} "
              "bloques ({} MB); tipo de memoria {}, unidad {} KB, alineacion que pide el driver {} KB",
              slabs_.size(), slab_mb, (slabs_.size() * slab_bytes_) >> 20, slabs_tope_,
              (uint64_t(slabs_tope_) * slab_bytes_) >> 20, tipo_memoria_, kUnidadPoolBytes >> 10,
              alineacion_vista >> 10);
  return true;
}

bool PoolTexturas::CrearSlab(bool en_caliente) {
  if (!activo_ || slabs_.size() >= slabs_tope_ || slabs_.size() >= kMaxSlabs) {
    return false;
  }
  /*
   * Without VkMemoryDedicatedAllocateInfo on purpose: that way the nvkmd_mem is created with
   * pte_kind = 0 and tile_mode = 0, its layout.valid stays false
   * (nvkmd/switch/nvkmd_switch_dev.c:787-795) and the block accepts aliased images with any
   * pte_kind (horizon/nouveau_horizon_memory.c:1056-1073).
   */
  VkMemoryAllocateInfo reserva{};
  reserva.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  reserva.allocationSize = slab_bytes_;
  reserva.memoryTypeIndex = tipo_memoria_;

  VkDeviceMemory memoria = VK_NULL_HANDLE;
  if (dispositivo_->functions().vkAllocateMemory(device_, &reserva, nullptr, &memoria) != VK_SUCCESS) {
    ++slabs_fallados_;
    return false;
  }

  Slab slab;
  slab.memoria = memoria;
  slab.unidades = slab_unidades_;
  slab.ocupadas.assign((slab_unidades_ + 63) / 64, 0ull);
  slab.largo.assign(slab_unidades_, 0u);
  slabs_.push_back(std::move(slab));
  if (en_caliente) {
    ++slabs_en_caliente_;
    REXLOG_INFO("[nativo] C3: pool de texturas: bloque {} nuevo en caliente; ya van {} MB reservados",
                slabs_.size() - 1, (slabs_.size() * slab_bytes_) >> 20);
  }
  return true;
}

void PoolTexturas::Terminar() {
  if (device_ != VK_NULL_HANDLE) {
    const auto& dfn = dispositivo_->functions();
    for (Slab& slab : slabs_) {
      if (slab.memoria != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device_, slab.memoria, nullptr);
      }
    }
  }
  slabs_.clear();
  activo_ = false;
  dispositivo_ = nullptr;
  device_ = VK_NULL_HANDLE;
}

bool PoolTexturas::Reservar(const VkMemoryRequirements& requisitos, VkDeviceMemory& memoria_out,
                            VkDeviceSize& offset_out, uint32_t& bloque_out) {
  if (!activo_) {
    return false;
  }
  // The slab's type must be among the ones this image accepts. On NVK for Tegra it always
  // is, but if some format said otherwise, falling back to the dedicated path is better than
  // binding something invalid.
  if (((requisitos.memoryTypeBits >> tipo_memoria_) & 1u) == 0u) {
    return false;
  }
  if (requisitos.size == 0) {
    return false;
  }

  const uint64_t unidades64 = (requisitos.size + kUnidadPoolBytes - 1) / kUnidadPoolBytes;
  if (unidades64 > slab_unidades_) {
    ++huecos_fallados_;
    return false;  // does not fit even in an empty block: use the dedicated path
  }
  const uint32_t unidades = uint32_t(unidades64);

  /*
   * The alignment is honored exactly; it is not assumed to be 64 KiB.
   *
   * For TILING_OPTIMAL on Switch the driver asks for at least 64 KiB
   * (nvk_image.c:1192-1197), but a 3D texture can ask for up to 512 KB because the level-0
   * tile size includes z_log2 (nouveau/nil/image.rs:430). Assuming 64 KiB would make the
   * bind fail on a driver assert (nvk_image.c:1692), a rare and late failure. Converted to
   * units, the alignment is the step used to search for candidate starts.
   */
  const uint64_t alineacion = std::max<uint64_t>(requisitos.alignment, kUnidadPoolBytes);
  const uint32_t paso = uint32_t(std::max<uint64_t>(1, alineacion / kUnidadPoolBytes));

  for (uint32_t s = 0; s < slabs_.size(); ++s) {
    Slab& slab = slabs_[s];
    uint32_t inicio = 0;
    if (!BuscarHueco(slab.ocupadas, slab.unidades, unidades, paso, inicio)) {
      continue;
    }
    MarcarRango(slab.ocupadas, inicio, unidades, true);
    slab.largo[inicio] = unidades;
    slab.unidades_en_uso += unidades;

    memoria_out = slab.memoria;
    offset_out = VkDeviceSize(inicio) * kUnidadPoolBytes;
    bloque_out = (s << 24) | inicio;
    ++texturas_vivas_;
    ++texturas_colocadas_;
    return true;
  }

  ++huecos_fallados_;
  return false;
}

void PoolTexturas::Liberar(uint32_t bloque) {
  if (bloque == kBloquePoolInvalido || !activo_) {
    return;
  }
  const uint32_t s = bloque >> 24;
  const uint32_t inicio = bloque & 0x00FFFFFFu;
  if (s >= slabs_.size()) {
    REXLOG_ERROR("[nativo] C3: pool de texturas: bloque {:08X} con un bloque grande que no existe", bloque);
    return;
  }
  Slab& slab = slabs_[s];
  if (inicio >= slab.unidades || slab.largo[inicio] == 0u) {
    REXLOG_ERROR("[nativo] C3: pool de texturas: bloque {:08X} que no era el inicio de nada (doble liberacion?)",
                 bloque);
    return;
  }
  const uint32_t unidades = slab.largo[inicio];
  MarcarRango(slab.ocupadas, inicio, unidades, false);
  slab.largo[inicio] = 0u;
  slab.unidades_en_uso -= std::min(slab.unidades_en_uso, unidades);
  if (texturas_vivas_ > 0) {
    --texturas_vivas_;
  }
}

void PoolTexturas::PorFotograma(uint64_t fotograma) {
  if (!activo_ || slabs_.size() >= slabs_tope_) {
    return;
  }
  if (fotograma < ultimo_crecimiento_ + kFotogramasEntreSlabs) {
    return;
  }
  uint64_t libres = 0;
  for (const Slab& slab : slabs_) {
    libres += uint64_t(slab.unidades - slab.unidades_en_uso) * kUnidadPoolBytes;
  }
  if (libres >= kHolguraBytes) {
    return;
  }
  ultimo_crecimiento_ = fotograma;
  CrearSlab(true);
}

EstadoPoolTexturas PoolTexturas::Estado() const {
  EstadoPoolTexturas e;
  e.activo = activo_;
  e.tipo_memoria = tipo_memoria_;
  e.slabs = uint32_t(slabs_.size());
  e.bytes_reservados = uint64_t(slabs_.size()) * slab_bytes_;
  uint32_t mayor = 0;
  for (const Slab& slab : slabs_) {
    e.bytes_en_uso += uint64_t(slab.unidades_en_uso) * kUnidadPoolBytes;
    mayor = std::max(mayor, MayorHueco(slab.ocupadas, slab.unidades));
  }
  e.bytes_libres = e.bytes_reservados - e.bytes_en_uso;
  e.bytes_mayor_hueco = uint64_t(mayor) * kUnidadPoolBytes;
  e.texturas_vivas = texturas_vivas_;
  e.texturas_colocadas = texturas_colocadas_;
  e.texturas_dedicadas = texturas_dedicadas_;
  e.huecos_fallados = huecos_fallados_;
  e.slabs_en_caliente = slabs_en_caliente_;
  e.slabs_fallados = slabs_fallados_;
  return e;
}

std::string PoolTexturas::Resumen() const {
  if (!activo_) {
    return std::string("pool de texturas apagado (") + std::to_string(texturas_dedicadas_) +
           " texturas con reserva dedicada)";
  }
  const EstadoPoolTexturas e = Estado();
  /*
   * Fragmentation is what needs watching: if plenty is free but the largest free range is
   * small, large textures start falling back to the dedicated path and the savings vanish
   * without anything failing. That is why both figures are printed together.
   */
  const uint64_t frag = e.bytes_libres > 0 ? 100u - (e.bytes_mayor_hueco * 100u / e.bytes_libres) : 0u;
  return std::string("pool de texturas: ") + std::to_string(e.slabs) + " bloques (" +
         std::to_string(e.bytes_reservados >> 20) + " MB), " + std::to_string(e.bytes_en_uso >> 20) +
         " MB en uso por " + std::to_string(e.texturas_vivas) + " texturas; libres " +
         std::to_string(e.bytes_libres >> 20) + " MB con el mayor hueco en " +
         std::to_string(e.bytes_mayor_hueco >> 20) + " MB (fragmentacion " + std::to_string(frag) +
         " %); " + std::to_string(e.texturas_colocadas) + " colocadas y " +
         std::to_string(e.texturas_dedicadas) + " a la ruta dedicada (" + std::to_string(e.huecos_fallados) +
         " sin hueco); bloques en caliente " + std::to_string(e.slabs_en_caliente) + ", negados " +
         std::to_string(e.slabs_fallados);
}

}  // namespace nfsmw::nativo
