#pragma once

#include <rex/cvar.h>
#include <vulkan/vulkan.h>

REXCVAR_DECLARE(bool, nfsmw_nativo_sincronizacion_gpu);

namespace nfsmw::nativo {

// GENERAL is a layout, not a memory dependency. The native renderer shares
// images between uploads, resolves, reflections and subsequent render passes.
inline constexpr VkPipelineStageFlags kEtapasImagenes =
    VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
inline constexpr VkAccessFlags kAccesosImagenes =
    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
inline constexpr VkPipelineStageFlags kEtapasPase =
    kEtapasImagenes & ~VK_PIPELINE_STAGE_TRANSFER_BIT;
inline constexpr VkAccessFlags kAccesosPase =
    kAccesosImagenes & ~(VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);

inline void DependenciasImagenes(VkSubpassDependency (&dependencias)[2]) {
  dependencias[0] = {VK_SUBPASS_EXTERNAL, 0, kEtapasImagenes, kEtapasPase,
                     kAccesosImagenes, kAccesosPase, 0};
  dependencias[1] = {0, VK_SUBPASS_EXTERNAL, kEtapasPase, kEtapasImagenes,
                     kAccesosPase, kAccesosImagenes, 0};
}

}  // namespace nfsmw::nativo
