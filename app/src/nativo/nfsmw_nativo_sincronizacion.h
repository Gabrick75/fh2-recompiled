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

// vertices_esperan = false (fh2_nativo_vertices_sin_espera): the vertex stage is left out of what waits, on both
// sides, so a tiler can bin the next pass's geometry while it still shades the previous pass. A vertex shader that
// samples an image written before then needs its own barrier (DibujosVulkanImpl::BarreraVertices).
inline void DependenciasImagenes(VkSubpassDependency (&dependencias)[2], bool vertices_esperan = true) {
  const VkPipelineStageFlags sin_vertices = vertices_esperan ? 0u : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
  dependencias[0] = {VK_SUBPASS_EXTERNAL, 0, kEtapasImagenes, kEtapasPase & ~sin_vertices,
                     kAccesosImagenes, kAccesosPase, 0};
  dependencias[1] = {0, VK_SUBPASS_EXTERNAL, kEtapasPase, kEtapasImagenes & ~sin_vertices,
                     kAccesosPase, kAccesosImagenes, 0};
}

}  // namespace nfsmw::nativo
