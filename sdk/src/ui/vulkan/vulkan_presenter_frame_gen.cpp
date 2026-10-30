/**
 ******************************************************************************
 * ReXGlue SDK                                                                *
 ******************************************************************************
 * present_frame_gen: a frame in the middle of every two guest frames.        *
 ******************************************************************************
 */

/*
 * How the frame in the middle is made (see FrameGen in presenter.h for the pieces, and
 * Presenter::PaintFromPaintThread for when it is presented).
 *
 * Only the two images are used, the way the frame generation that phones offer for any game works: no
 * depth and no motion vectors from the game, so this works with both renderers. Per guest frame:
 *   1. Its luma at 1/4, 1/8, 1/16 and 1/32 (one pyramid per frame, two kept).
 *   2. One vector per texel of 1/32, searching every whole texel within kSearchRange; then at 1/16, 1/8 and
 *      1/4, the five nearest vectors of the level above plus zero, refined one step around the best.
 *      The search is symmetric (frame_gen_search.ps.glsl), so the vectors belong to the frame in the
 *      middle and it has no holes. A 3x3 vector median of the last level drops the outliers
 *      (frame_gen_median.ps.glsl).
 *   3. Per guest pixel, the best of six vectors (frame_gen_interpolate.ps.glsl), the smooth field first,
 *      and the average of its two ends; the current frame unwarped where nothing matches.
 * The real frame is then kept as the history (a copy: the mailbox reuses its images).
 *
 * Measured on the S23 (Adreno 740, 1280x720, three levels): 1.5-2.2 ms of GPU per frame in the middle.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/ui_samplers.h>
#include <rex/ui/vulkan/util.h>

REXCVAR_DECLARE(bool, present_frame_gen);  // defined in presenter.cpp

/*
 * Debugging: the captures (fh2_captura_cada_s, the native renderer's) save the last frame in the middle
 * instead of the real one, to look at what the search and the interpolation made. The capture thread
 * reads the image while the presenter thread may be drawing the next one into it: a capture can mix two
 * frames in the middle, which is fine for looking at them and is why this is not for players.
 */
/*
 * Debugging, to tell a pacing fault from an interpolation fault: 1 presents the real frame where the one in
 * the middle would go (the same pace, nothing interpolated); 2 presents the frame in the middle in place of
 * the real one too (only interpolated frames, at the game's rate).
 */
REXCVAR_DEFINE_INT32(present_frame_gen_depurar, 0, "UI/Presenter",
                     "Depuracion de present_frame_gen: 0 normal, 1 el fotograma real en lugar del intermedio "
                     "(mismo ritmo, sin interpolar), 2 solo los intermedios")
    .range(0, 2);

/*
 * Debugging: every this many seconds, the previous frame, the one in the middle and the current one of the
 * same paint, copied in its command buffer (so the three belong together), as PPM files in
 * present_frame_gen_volcar_dir. Up to 20 per run. They are what the search and the interpolation can be
 * tuned against on a PC.
 */
REXCVAR_DEFINE_INT32(present_frame_gen_volcar_cada_s, 0, "UI/Presenter",
                     "Depuracion de present_frame_gen: cada cuantos segundos guardar el fotograma anterior, el "
                     "intermedio y el actual (0 = nunca)");
REXCVAR_DEFINE_STRING(present_frame_gen_volcar_dir, "", "UI/Presenter",
                      "Depuracion de present_frame_gen: carpeta de los volcados (vacia = la del ejecutable)");

REXCVAR_DEFINE_BOOL(present_frame_gen_capturar_intermedio, false, "UI/Presenter",
                    "Depuracion de present_frame_gen: las capturas guardan el ultimo fotograma intermedio "
                    "en vez del real");

namespace rex {
namespace ui {
namespace vulkan {

// tools/compilar_shaders.sh, from ../shaders/frame_gen_*.ps.glsl.
namespace shaders {
#include "../shaders/vulkan_spirv/frame_gen_copy_ps.h"
#include "../shaders/vulkan_spirv/frame_gen_interpolate_ps.h"
#include "../shaders/vulkan_spirv/frame_gen_luma_ps.h"
#include "../shaders/vulkan_spirv/frame_gen_median_ps.h"
#include "../shaders/vulkan_spirv/frame_gen_search_ps.h"
}  // namespace shaders

namespace {

// The push constants of the fragment shaders, after the 16 bytes of the vertex shader's rectangle.
struct LumaConstants {
  float source_texel[2];
  float dest_texel[2];
  int32_t from_guest_output;
};
struct SearchConstants {
  float level_texel[2];
  float parent_texel[2];
  float guest_pixels_per_texel;
  float refine_step;
  int32_t search_range;
};
struct MedianConstants {
  float vector_texel[2];
};
struct InterpolateConstants {
  float guest_texel[2];
  float vector_size[2];
};
constexpr uint32_t kFragmentConstantsSize = 32;
static_assert(sizeof(LumaConstants) <= kFragmentConstantsSize);
static_assert(sizeof(SearchConstants) <= kFragmentConstantsSize);
static_assert(sizeof(InterpolateConstants) <= kFragmentConstantsSize);

constexpr VkFormat kLumaFormat = VK_FORMAT_R8_UNORM;
constexpr VkFormat kVectorFormat = VK_FORMAT_R16G16_SFLOAT;

// Texels of 1/32 searched each way: +-192 guest pixels for each half of the motion, so up to 384 pixels
// between two frames (the roadside at full speed moved past the 192 of a search at 1/16).
constexpr int32_t kSearchRange = 6;

// The levels' sizes: 1/4, 1/8, 1/16 and 1/32 of the guest output, rounded up.
uint32_t LevelSize(uint32_t guest_size, uint32_t level) {
  const uint32_t shift = 2 + level;
  return std::max(uint32_t(1), (guest_size + (uint32_t(1) << shift) - 1) >> shift);
}

}  // namespace

bool VulkanPresenter::IsFrameGenAvailable() const {
  return REXCVAR_GET(present_frame_gen) && !frame_gen_.failed;
}

std::shared_ptr<VulkanPresenter::GuestOutputImage> VulkanPresenter::FrameGenCaptureImage() const {
  if (!REXCVAR_GET(present_frame_gen_capturar_intermedio) || !frame_gen_.intermediates) {
    return nullptr;
  }
  return frame_gen_.output;
}

int32_t VulkanPresenter::FrameGenDebugMode() const { return REXCVAR_GET(present_frame_gen_depurar); }

bool VulkanPresenter::FrameGenInitialize() {
  if (frame_gen_.initialized || frame_gen_.failed) {
    return frame_gen_.initialized;
  }
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  FrameGen& fg = frame_gen_;
  const auto fail = [&](const char* what) {
    REXLOG_ERROR("[presentador] frame gen: no se pudo crear {}; queda apagado", what);
    fg.failed = true;
    FrameGenShutdown();
    return false;
  };

  // Render passes: each pass writes its whole target and leaves it for sampling, like the intermediate
  // images of the effects.
  const auto create_render_pass = [&](VkFormat format, VkRenderPass& render_pass) {
    VkAttachmentDescription attachment = {};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference color_attachment = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_attachment;
    // Earlier reads (this frame's or an earlier paint's) before the write, and the write before the
    // reads that follow: the next pass, or the effects reading the frame in the middle (fragment, and
    // compute for a temporal upscaler).
    VkSubpassDependency dependencies[2] = {};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    create_info.attachmentCount = 1;
    create_info.pAttachments = &attachment;
    create_info.subpassCount = 1;
    create_info.pSubpasses = &subpass;
    create_info.dependencyCount = 2;
    create_info.pDependencies = dependencies;
    return dfn.vkCreateRenderPass(device, &create_info, nullptr, &render_pass) == VK_SUCCESS;
  };
  if (!create_render_pass(kLumaFormat, fg.luma_pass) ||
      !create_render_pass(kVectorFormat, fg.vector_pass) ||
      !create_render_pass(kGuestOutputFormat, fg.color_pass)) {
    return fail("los render passes");
  }

  // Set 1: three images and the two samplers.
  const VkSampler linear_sampler =
      ui_samplers_->samplers()[UISamplers::kSamplerIndexLinearClampToEdge];
  const VkSampler nearest_sampler =
      ui_samplers_->samplers()[UISamplers::kSamplerIndexNearestClampToEdge];
  VkDescriptorSetLayoutBinding bindings[5] = {};
  for (uint32_t i = 0; i < 5; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
  }
  bindings[3].pImmutableSamplers = &linear_sampler;
  bindings[4].pImmutableSamplers = &nearest_sampler;
  VkDescriptorSetLayoutCreateInfo set_layout_info = {};
  set_layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  set_layout_info.bindingCount = 5;
  set_layout_info.pBindings = bindings;
  if (dfn.vkCreateDescriptorSetLayout(device, &set_layout_info, nullptr, &fg.set_layout) !=
      VK_SUCCESS) {
    return fail("el layout de descriptores");
  }

  // Set 0 is the guest output's paint descriptor set as it is.
  const VkDescriptorSetLayout set_layouts[2] = {guest_output_paint_image_descriptor_set_layout_,
                                                fg.set_layout};
  VkPushConstantRange push_constant_ranges[2] = {};
  push_constant_ranges[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push_constant_ranges[0].size = sizeof(GuestOutputPaintRectangleConstants);
  push_constant_ranges[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push_constant_ranges[1].offset = sizeof(GuestOutputPaintRectangleConstants);
  push_constant_ranges[1].size = kFragmentConstantsSize;
  VkPipelineLayoutCreateInfo pipeline_layout_info = {};
  pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_info.setLayoutCount = 2;
  pipeline_layout_info.pSetLayouts = set_layouts;
  pipeline_layout_info.pushConstantRangeCount = 2;
  pipeline_layout_info.pPushConstantRanges = push_constant_ranges;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_info, nullptr, &fg.pipeline_layout) !=
      VK_SUCCESS) {
    return fail("el layout de las tuberias");
  }

  const auto create_shader = [&](const uint32_t* code, size_t size, VkShaderModule& module) {
    VkShaderModuleCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    create_info.codeSize = size;
    create_info.pCode = code;
    return dfn.vkCreateShaderModule(device, &create_info, nullptr, &module) == VK_SUCCESS;
  };
  if (!create_shader(shaders::frame_gen_luma_ps, sizeof(shaders::frame_gen_luma_ps), fg.luma_fs) ||
      !create_shader(shaders::frame_gen_search_ps, sizeof(shaders::frame_gen_search_ps),
                     fg.search_fs) ||
      !create_shader(shaders::frame_gen_interpolate_ps, sizeof(shaders::frame_gen_interpolate_ps),
                     fg.interpolate_fs) ||
      !create_shader(shaders::frame_gen_copy_ps, sizeof(shaders::frame_gen_copy_ps), fg.copy_fs) ||
      !create_shader(shaders::frame_gen_median_ps, sizeof(shaders::frame_gen_median_ps),
                     fg.median_fs)) {
    return fail("los shaders");
  }
  // The effects' vertex shader (a rectangle from push constants) with these fragment shaders.
  fg.luma_pipeline = CreateGuestOutputPaintPipeline(GuestOutputPaintEffect::kBilinear, fg.luma_pass,
                                                    fg.luma_fs, fg.pipeline_layout);
  fg.search_pipeline = CreateGuestOutputPaintPipeline(
      GuestOutputPaintEffect::kBilinear, fg.vector_pass, fg.search_fs, fg.pipeline_layout);
  fg.interpolate_pipeline = CreateGuestOutputPaintPipeline(
      GuestOutputPaintEffect::kBilinear, fg.color_pass, fg.interpolate_fs, fg.pipeline_layout);
  fg.copy_pipeline = CreateGuestOutputPaintPipeline(GuestOutputPaintEffect::kBilinear, fg.color_pass,
                                                    fg.copy_fs, fg.pipeline_layout);
  fg.median_pipeline = CreateGuestOutputPaintPipeline(
      GuestOutputPaintEffect::kBilinear, fg.vector_pass, fg.median_fs, fg.pipeline_layout);
  if (fg.luma_pipeline == VK_NULL_HANDLE || fg.search_pipeline == VK_NULL_HANDLE ||
      fg.median_pipeline == VK_NULL_HANDLE ||
      fg.interpolate_pipeline == VK_NULL_HANDLE || fg.copy_pipeline == VK_NULL_HANDLE) {
    return fail("las tuberias");
  }

  constexpr uint32_t kSetCount = 2 * kFrameGenLevels * 2 + 2;
  VkDescriptorPoolSize pool_sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetCount * 3},
                                        {VK_DESCRIPTOR_TYPE_SAMPLER, kSetCount * 2}};
  VkDescriptorPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool_info.maxSets = kSetCount;
  pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &pool_info, nullptr, &fg.descriptor_pool) != VK_SUCCESS) {
    return fail("el pool de descriptores");
  }
  VkDescriptorSet* sets[kSetCount];
  uint32_t set_index = 0;
  for (uint32_t pyramid = 0; pyramid < 2; ++pyramid) {
    for (uint32_t level = 0; level < kFrameGenLevels; ++level) {
      sets[set_index++] = &fg.luma_sets[pyramid][level];
      sets[set_index++] = &fg.search_sets[pyramid][level];
    }
  }
  sets[set_index++] = &fg.interpolate_set;
  sets[set_index++] = &fg.median_set;
  for (VkDescriptorSet* set : sets) {
    VkDescriptorSetAllocateInfo allocate_info = {};
    allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate_info.descriptorPool = fg.descriptor_pool;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &fg.set_layout;
    if (dfn.vkAllocateDescriptorSets(device, &allocate_info, set) != VK_SUCCESS) {
      return fail("los descriptores");
    }
  }

  // Timestamps, where the paint queue has them (not needed to work).
  {
    const VulkanInstance::Functions& ifn = vulkan_device_->vulkan_instance()->functions();
    uint32_t family_count = 0;
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &family_count,
                                                 nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device_->physical_device(), &family_count,
                                                 families.data());
    VkPhysicalDeviceProperties properties = {};
    ifn.vkGetPhysicalDeviceProperties(vulkan_device_->physical_device(), &properties);
    const uint32_t family = vulkan_device_->queue_family_graphics_compute();
    if (family < family_count && families[family].timestampValidBits &&
        properties.limits.timestampPeriod > 0.0f) {
      VkQueryPoolCreateInfo pool_info = {};
      pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
      pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      pool_info.queryCount = 2 * FrameGen::kTimestampSlots;
      if (dfn.vkCreateQueryPool(device, &pool_info, nullptr, &fg.timestamps) != VK_SUCCESS) {
        fg.timestamps = VK_NULL_HANDLE;
      }
      fg.timestamp_period_ns = properties.limits.timestampPeriod;
    }
  }

  fg.initialized = true;
  REXLOG_INFO("[presentador] frame gen: listo (un fotograma intermedio entre cada dos del juego; tiempo de "
              "GPU {})",
              fg.timestamps != VK_NULL_HANDLE ? "medido" : "sin medir: la cola no tiene marcas de tiempo");
  return true;
}

void VulkanPresenter::FrameGenDestroyTargets() {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  FrameGen& fg = frame_gen_;
  const auto destroy = [&](FrameGenTarget& target) {
    util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, target.framebuffer);
    util::DestroyAndNullHandle(dfn.vkDestroyImageView, device, target.view);
    util::DestroyAndNullHandle(dfn.vkDestroyImage, device, target.image);
    util::DestroyAndNullHandle(dfn.vkFreeMemory, device, target.memory);
    target.width = 0;
    target.height = 0;
  };
  destroy(fg.history);
  for (auto& pyramid : fg.luma) {
    for (FrameGenTarget& level : pyramid) {
      destroy(level);
    }
  }
  for (FrameGenTarget& level : fg.vectors) {
    destroy(level);
  }
  destroy(fg.smoothed);
  util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device, fg.output_framebuffer);
  fg.output.reset();
  util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, fg.dump_buffer);
  util::DestroyAndNullHandle(dfn.vkFreeMemory, device, fg.dump_memory);
  fg.dump_pending = false;
  paint_context_.guest_output_image_paint_refs[PaintContext::kGuestOutputPaintRefFrameGen] = {};
  fg.width = 0;
  fg.height = 0;
  fg.luma_sequence[0] = 0;
  fg.luma_sequence[1] = 0;
  fg.history_sequence = 0;
  fg.output_sequence = 0;
}

void VulkanPresenter::FrameGenShutdown() {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  FrameGen& fg = frame_gen_;
  FrameGenDestroyTargets();
  util::DestroyAndNullHandle(dfn.vkDestroyQueryPool, device, fg.timestamps);
  for (bool& written : fg.timestamp_written) {
    written = false;
  }
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device, fg.descriptor_pool);
  util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fg.median_pipeline);
  util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fg.copy_pipeline);
  util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fg.interpolate_pipeline);
  util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fg.search_pipeline);
  util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, fg.luma_pipeline);
  util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, fg.median_fs);
  util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, fg.copy_fs);
  util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, fg.interpolate_fs);
  util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, fg.search_fs);
  util::DestroyAndNullHandle(dfn.vkDestroyShaderModule, device, fg.luma_fs);
  util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device, fg.pipeline_layout);
  util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device, fg.set_layout);
  util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, fg.color_pass);
  util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, fg.vector_pass);
  util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device, fg.luma_pass);
  fg.initialized = false;
}

bool VulkanPresenter::FrameGenEnsureSize(VkCommandBuffer command_buffer, uint32_t width,
                                         uint32_t height) {
  FrameGen& fg = frame_gen_;
  if (!fg.initialized && !FrameGenInitialize()) {
    return false;
  }
  if (fg.width == width && fg.height == height) {
    return true;
  }
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  // Earlier paints may still read the targets and their descriptors.
  paint_context_.submission_tracker.AwaitAllSubmissionsCompletion();
  FrameGenDestroyTargets();

  const auto create_target = [&](FrameGenTarget& target, uint32_t target_width,
                                 uint32_t target_height, VkFormat format,
                                 VkRenderPass render_pass, VkImageUsageFlags extra_usage = 0) {
    VkImageCreateInfo image_info = {};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent = {target_width, target_height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | extra_usage;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!util::CreateDedicatedAllocationImage(vulkan_device_, image_info,
                                              util::MemoryPurpose::kDeviceLocal, target.image,
                                              target.memory)) {
      return false;
    }
    VkImageViewCreateInfo view_info = {};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = target.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (dfn.vkCreateImageView(device, &view_info, nullptr, &target.view) != VK_SUCCESS) {
      return false;
    }
    VkFramebufferCreateInfo framebuffer_info = {};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &target.view;
    framebuffer_info.width = target_width;
    framebuffer_info.height = target_height;
    framebuffer_info.layers = 1;
    if (dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr, &target.framebuffer) !=
        VK_SUCCESS) {
      return false;
    }
    target.width = target_width;
    target.height = target_height;
    return true;
  };
  // (Transfer source for present_frame_gen_volcar_cada_s.)
  bool created = create_target(fg.history, width, height, kGuestOutputFormat, fg.color_pass,
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  for (uint32_t level = 0; created && level < kFrameGenLevels; ++level) {
    const uint32_t level_width = LevelSize(width, level), level_height = LevelSize(height, level);
    created = create_target(fg.luma[0][level], level_width, level_height, kLumaFormat,
                            fg.luma_pass) &&
              create_target(fg.luma[1][level], level_width, level_height, kLumaFormat,
                            fg.luma_pass) &&
              create_target(fg.vectors[level], level_width, level_height, kVectorFormat,
                            fg.vector_pass);
  }
  created = created && create_target(fg.smoothed, LevelSize(width, 0), LevelSize(height, 0),
                                     kVectorFormat, fg.vector_pass);
  if (created) {
    std::unique_ptr<GuestOutputImage> output = GuestOutputImage::Create(vulkan_device_, width, height);
    created = output != nullptr;
    if (created) {
      fg.output = std::move(output);
      const VkImageView output_view = fg.output->view();
      VkFramebufferCreateInfo framebuffer_info = {};
      framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      framebuffer_info.renderPass = fg.color_pass;
      framebuffer_info.attachmentCount = 1;
      framebuffer_info.pAttachments = &output_view;
      framebuffer_info.width = width;
      framebuffer_info.height = height;
      framebuffer_info.layers = 1;
      created = dfn.vkCreateFramebuffer(device, &framebuffer_info, nullptr,
                                        &fg.output_framebuffer) == VK_SUCCESS;
    }
  }
  if (!created) {
    REXLOG_ERROR("[presentador] frame gen: no se pudieron crear las imagenes de {}x{}; queda apagado",
                 width, height);
    fg.failed = true;
    FrameGenDestroyTargets();
    return false;
  }

  // The descriptors, all at once: nothing in flight reads them now.
  const auto write_set = [&](VkDescriptorSet set, const FrameGenTarget& a, const FrameGenTarget& b,
                             const FrameGenTarget& c) {
    VkDescriptorImageInfo image_infos[3] = {
        {VK_NULL_HANDLE, a.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, b.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {VK_NULL_HANDLE, c.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 3;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = image_infos;
    dfn.vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  };
  // Unused bindings point at an image that is already written when the pass runs (and never at the
  // pass's own target).
  for (uint32_t pyramid = 0; pyramid < 2; ++pyramid) {
    for (uint32_t level = 0; level < kFrameGenLevels; ++level) {
      // Level 0 reads the guest output; its binding points at the level below instead of itself.
      const FrameGenTarget& above = fg.luma[pyramid][level ? level - 1 : 1];
      write_set(fg.luma_sets[pyramid][level], above, above, above);
      const FrameGenTarget& current = fg.luma[pyramid][level];
      write_set(fg.search_sets[pyramid][level], fg.luma[pyramid ^ 1][level], current,
                level + 1 < kFrameGenLevels ? fg.vectors[level + 1] : current);
    }
  }
  write_set(fg.median_set, fg.vectors[0], fg.vectors[0], fg.vectors[0]);
  write_set(fg.interpolate_set, fg.history, fg.history, fg.smoothed);
  // The frame in the middle as a guest output image for the effects, in its own reference slot.
  VkDescriptorImageInfo output_info = {VK_NULL_HANDLE, fg.output->view(),
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet output_write = {};
  output_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  output_write.dstSet =
      paint_context_.guest_output_descriptor_sets[PaintContext::kGuestOutputDescriptorSetGuestOutput0Sampled +
                                                  PaintContext::kGuestOutputPaintRefFrameGen];
  output_write.dstBinding = 0;
  output_write.descriptorCount = 1;
  output_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  output_write.pImageInfo = &output_info;
  dfn.vkUpdateDescriptorSets(device, 1, &output_write, 0, nullptr);

  // Every new image to the layout the descriptors say, so a binding a pass does not read is valid from
  // the first frame.
  VkImageMemoryBarrier barriers[1 + 3 * kFrameGenLevels + 2];
  uint32_t barrier_count = 0;
  const auto add_barrier = [&](VkImage image) {
    VkImageMemoryBarrier& barrier = barriers[barrier_count++];
    barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  };
  add_barrier(fg.history.image);
  for (uint32_t level = 0; level < kFrameGenLevels; ++level) {
    add_barrier(fg.luma[0][level].image);
    add_barrier(fg.luma[1][level].image);
    add_barrier(fg.vectors[level].image);
  }
  add_barrier(fg.smoothed.image);
  add_barrier(fg.output->image());
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                           barrier_count, barriers);

  fg.width = width;
  fg.height = height;
  REXLOG_INFO(
      "[presentador] frame gen: imagenes de {}x{} (busqueda en {}x{}, vectores de {}x{})", width,
      height, LevelSize(width, kFrameGenLevels - 1), LevelSize(height, kFrameGenLevels - 1),
      LevelSize(width, 0), LevelSize(height, 0));
  return true;
}

namespace {

// One fragment pass over the whole of a target.
void FrameGenPass(const VulkanDevice::Functions& dfn, VkCommandBuffer command_buffer,
                  VkRenderPass render_pass, VkFramebuffer framebuffer, uint32_t width,
                  uint32_t height, VkPipeline pipeline, VkPipelineLayout pipeline_layout,
                  VkDescriptorSet set_0, VkDescriptorSet set_1, const void* constants,
                  uint32_t constants_size) {
  VkRenderPassBeginInfo begin_info = {};
  begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin_info.renderPass = render_pass;
  begin_info.framebuffer = framebuffer;
  begin_info.renderArea.extent = {width, height};
  dfn.vkCmdBeginRenderPass(command_buffer, &begin_info, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport = {0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f};
  dfn.vkCmdSetViewport(command_buffer, 0, 1, &viewport);
  const VkRect2D scissor = {{0, 0}, {width, height}};
  dfn.vkCmdSetScissor(command_buffer, 0, 1, &scissor);
  dfn.vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  const VkDescriptorSet sets[2] = {set_0, set_1};
  dfn.vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 2,
                              sets, 0, nullptr);
  // The whole target, in NDC.
  const float rectangle[4] = {-1.0f, -1.0f, 2.0f, 2.0f};
  dfn.vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                         sizeof(rectangle), rectangle);
  if (constants_size) {
    dfn.vkCmdPushConstants(command_buffer, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           sizeof(rectangle), constants_size, constants);
  }
  dfn.vkCmdDraw(command_buffer, 4, 1, 0, 0);
  dfn.vkCmdEndRenderPass(command_buffer);
}

}  // namespace

void VulkanPresenter::FrameGenRecordLuma(VkCommandBuffer command_buffer,
                                         VkDescriptorSet guest_output_set, uint32_t pyramid) {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  FrameGen& fg = frame_gen_;
  for (uint32_t level = 0; level < kFrameGenLevels; ++level) {
    const FrameGenTarget& target = fg.luma[pyramid][level];
    LumaConstants constants;
    if (level) {
      const FrameGenTarget& above = fg.luma[pyramid][level - 1];
      constants.source_texel[0] = 1.0f / float(above.width);
      constants.source_texel[1] = 1.0f / float(above.height);
    } else {
      constants.source_texel[0] = 1.0f / float(fg.width);
      constants.source_texel[1] = 1.0f / float(fg.height);
    }
    constants.dest_texel[0] = 1.0f / float(target.width);
    constants.dest_texel[1] = 1.0f / float(target.height);
    constants.from_guest_output = level == 0;
    FrameGenPass(dfn, command_buffer, fg.luma_pass, target.framebuffer, target.width, target.height,
                 fg.luma_pipeline, fg.pipeline_layout, guest_output_set, fg.luma_sets[pyramid][level],
                 &constants, sizeof(constants));
  }
}

bool VulkanPresenter::FrameGenRecordIntermediate(VkCommandBuffer command_buffer,
                                                 VkDescriptorSet guest_output_set,
                                                 VkImage guest_output_image,
                                                 const FrameGenMotion& motion, uint64_t sequence) {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  FrameGen& fg = frame_gen_;
  // The previous frame: the history, and the pyramid made from it.
  uint32_t previous = UINT32_MAX;
  if (fg.history_sequence && fg.history_sequence < sequence) {
    for (uint32_t pyramid = 0; pyramid < 2; ++pyramid) {
      if (fg.luma_sequence[pyramid] == fg.history_sequence) {
        previous = pyramid;
      }
    }
  }
  if (previous == UINT32_MAX) {
    if (++fg.skipped % 60 == 1) {
      REXLOG_INFO("[presentador] frame gen: sin fotograma anterior ({} veces)", fg.skipped);
    }
    return false;
  }
  // The GPU time of the frame in the middle kTimestampSlots ago, and this one's first timestamp.
  const uint32_t slot = fg.timestamp_slot;
  if (fg.timestamps != VK_NULL_HANDLE) {
    fg.timestamp_slot = (slot + 1) % FrameGen::kTimestampSlots;
    if (fg.timestamp_written[slot]) {
      uint64_t ticks[2];
      if (dfn.vkGetQueryPoolResults(vulkan_device_->device(), fg.timestamps, slot * 2, 2, sizeof(ticks),
                                    ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
          ticks[1] >= ticks[0]) {
        fg.gpu_ms_sum += double(ticks[1] - ticks[0]) * fg.timestamp_period_ns / 1e6;
        ++fg.gpu_ms_count;
      }
    }
    dfn.vkCmdResetQueryPool(command_buffer, fg.timestamps, slot * 2, 2);
    dfn.vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, fg.timestamps, slot * 2);
  }

  const uint32_t current = previous ^ 1;
  if (fg.luma_sequence[current] != sequence) {
    FrameGenRecordLuma(command_buffer, guest_output_set, current);
    fg.luma_sequence[current] = sequence;
  }

  // The search, from the coarsest level down.
  for (uint32_t level = kFrameGenLevels; level-- > 0;) {
    const FrameGenTarget& target = fg.vectors[level];
    SearchConstants constants = {};
    constants.level_texel[0] = 1.0f / float(target.width);
    constants.level_texel[1] = 1.0f / float(target.height);
    constants.guest_pixels_per_texel = float(fg.width) / float(target.width);
    if (level + 1 < kFrameGenLevels) {
      constants.parent_texel[0] = 1.0f / float(fg.vectors[level + 1].width);
      constants.parent_texel[1] = 1.0f / float(fg.vectors[level + 1].height);
      // A whole texel at 1/8, half a texel (two guest pixels) at 1/4.
      constants.refine_step = level ? 1.0f : 0.5f;
    } else {
      constants.search_range = kSearchRange;
    }
    FrameGenPass(dfn, command_buffer, fg.vector_pass, target.framebuffer, target.width,
                 target.height, fg.search_pipeline, fg.pipeline_layout, guest_output_set,
                 fg.search_sets[current][level], &constants, sizeof(constants));
  }

  // The vector median of the finest level.
  MedianConstants median_constants;
  median_constants.vector_texel[0] = 1.0f / float(fg.smoothed.width);
  median_constants.vector_texel[1] = 1.0f / float(fg.smoothed.height);
  FrameGenPass(dfn, command_buffer, fg.vector_pass, fg.smoothed.framebuffer, fg.smoothed.width,
               fg.smoothed.height, fg.median_pipeline, fg.pipeline_layout, guest_output_set,
               fg.median_set, &median_constants, sizeof(median_constants));

  InterpolateConstants constants;
  constants.guest_texel[0] = 1.0f / float(fg.width);
  constants.guest_texel[1] = 1.0f / float(fg.height);
  constants.vector_size[0] = float(fg.smoothed.width);
  constants.vector_size[1] = float(fg.smoothed.height);
  FrameGenPass(dfn, command_buffer, fg.color_pass, fg.output_framebuffer, fg.width, fg.height,
               fg.interpolate_pipeline, fg.pipeline_layout, guest_output_set, fg.interpolate_set,
               &constants, sizeof(constants));
  fg.output_sequence = sequence;
  FrameGenDump(command_buffer, guest_output_image, motion);
  if (fg.timestamps != VK_NULL_HANDLE) {
    dfn.vkCmdWriteTimestamp(command_buffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, fg.timestamps,
                            slot * 2 + 1);
    fg.timestamp_written[slot] = true;
  }
  // Every ~30 s at 60 on the screen.
  if (++fg.intermediates % 900 == 0) {
    REXLOG_INFO(
        "[presentador] frame gen: {} fotogramas intermedios, {} sin fotograma anterior; GPU {:.2f} ms "
        "por intermedio ({} medidos)",
        fg.intermediates, fg.skipped, fg.gpu_ms_count ? fg.gpu_ms_sum / double(fg.gpu_ms_count) : 0.0,
        fg.gpu_ms_count);
    fg.gpu_ms_sum = 0.0;
    fg.gpu_ms_count = 0;
  }
  return true;
}

void VulkanPresenter::FrameGenDump(VkCommandBuffer command_buffer, VkImage guest_output_image,
                                   const FrameGenMotion& motion) {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  const VkDevice device = vulkan_device_->device();
  FrameGen& fg = frame_gen_;
  constexpr uint32_t kMaxDumps = 20;

  // The previous dump, once its paint has completed.
  if (fg.dump_pending &&
      paint_context_.submission_tracker.UpdateAndGetCompletedSubmission() >= fg.dump_submission) {
    fg.dump_pending = false;
    void* mapping = nullptr;
    if (dfn.vkMapMemory(device, fg.dump_memory, 0, VK_WHOLE_SIZE, 0, &mapping) == VK_SUCCESS) {
      const std::string dir = REXCVAR_GET(present_frame_gen_volcar_dir);
      const size_t pixels = size_t(fg.dump_width) * fg.dump_height;
      static const char* const kNames[3] = {"anterior", "intermedio", "actual"};
      for (uint32_t i = 0; i < 3; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "fg_%02u_%s.ppm", fg.dumps, kNames[i]);
        const std::string path = dir.empty() ? std::string(name) : dir + "/" + name;
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
          std::fprintf(file, "P6\n%u %u\n255\n", fg.dump_width, fg.dump_height);
          const uint32_t* source = static_cast<const uint32_t*>(mapping) + pixels * i;
          std::vector<uint8_t> row(size_t(fg.dump_width) * 3);
          for (uint32_t y = 0; y < fg.dump_height; ++y) {
            for (uint32_t x = 0; x < fg.dump_width; ++x) {
              const uint32_t rgba = Packed10bpcRGBTo8bpcBytes(source[size_t(y) * fg.dump_width + x]);
              row[x * 3 + 0] = uint8_t(rgba);
              row[x * 3 + 1] = uint8_t(rgba >> 8);
              row[x * 3 + 2] = uint8_t(rgba >> 16);
            }
            std::fwrite(row.data(), 1, row.size(), file);
          }
          std::fclose(file);
        } else {
          REXLOG_WARN("[presentador] frame gen: no se pudo escribir {}", path);
        }
      }
      // The game's velocity as it is: a small header (size, VkFormat, motionBlurParams1), then 4 bytes per texel.
      if (fg.dump_motion_width) {
        char name[64];
        std::snprintf(name, sizeof(name), "fg_%02u_velocidad.bin", fg.dumps);
        const std::string path = dir.empty() ? std::string(name) : dir + "/" + name;
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
          const uint32_t header[3] = {fg.dump_motion_width, fg.dump_motion_height,
                                      uint32_t(fg.dump_motion_format)};
          std::fwrite(header, sizeof(header), 1, file);
          std::fwrite(fg.dump_motion_params, sizeof(fg.dump_motion_params), 1, file);
          std::fwrite(static_cast<const uint8_t*>(mapping) + pixels * 4 * 3, 4,
                      size_t(fg.dump_motion_width) * fg.dump_motion_height, file);
          std::fclose(file);
        }
      }
      dfn.vkUnmapMemory(device, fg.dump_memory);
      REXLOG_INFO("[presentador] frame gen: volcado {} (anterior, intermedio y actual{})", fg.dumps,
                  fg.dump_motion_width ? ", con la velocidad del juego" : "");
      ++fg.dumps;
    }
  }

  // A new one, when due.
  const int32_t every_s = REXCVAR_GET(present_frame_gen_volcar_cada_s);
  const auto now = std::chrono::steady_clock::now();
  if (every_s <= 0 || fg.dump_pending || fg.dumps >= kMaxDumps ||
      (fg.last_dump != std::chrono::steady_clock::time_point{} &&
       now - fg.last_dump < std::chrono::seconds(every_s))) {
    return;
  }
  if (fg.dump_buffer == VK_NULL_HANDLE &&
      !util::CreateDedicatedAllocationBuffer(vulkan_device_, VkDeviceSize(fg.width) * fg.height * 4 * 4,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             util::MemoryPurpose::kReadback, fg.dump_buffer,
                                             fg.dump_memory)) {
    REXLOG_WARN("[presentador] frame gen: sin memoria para los volcados");
    fg.dumps = kMaxDumps;
    return;
  }
  fg.last_dump = now;
  fg.dump_width = fg.width;
  fg.dump_height = fg.height;
  // The velocity too, its top-left corner the size of the guest output (FH2's target is an EDRAM image taller
  // than the screen), when it has 4 bytes per texel.
  const bool with_motion =
      motion.valid() && (motion.format == VK_FORMAT_R8G8B8A8_UNORM || motion.format == VK_FORMAT_B8G8R8A8_UNORM ||
                         motion.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
  fg.dump_motion_width = with_motion ? std::min(motion.width, fg.width) : 0;
  fg.dump_motion_height = with_motion ? std::min(motion.height, fg.height) : 0;
  fg.dump_motion_format = motion.format;
  for (uint32_t i = 0; i < 4; ++i) {
    fg.dump_motion_params[i] = motion.params[i];
  }
  if (motion.valid() && !with_motion) {
    REXLOG_WARN("[presentador] frame gen: la velocidad del juego ({}x{}, VkFormat {}) no entra en el volcado",
                motion.width, motion.height, uint32_t(motion.format));
  }
  const VkImage images[3] = {fg.history.image, fg.output->image(), guest_output_image};
  VkImageMemoryBarrier barriers[3];
  for (uint32_t i = 0; i < 3; ++i) {
    VkImageMemoryBarrier& barrier = barriers[i];
    barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = images[i];
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  }
  dfn.vkCmdPipelineBarrier(command_buffer,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);
  for (uint32_t i = 0; i < 3; ++i) {
    VkBufferImageCopy region = {};
    region.bufferOffset = VkDeviceSize(fg.width) * fg.height * 4 * i;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {fg.width, fg.height, 1};
    dfn.vkCmdCopyImageToBuffer(command_buffer, images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               fg.dump_buffer, 1, &region);
  }
  if (with_motion) {
    // In GENERAL, written by the refresher's earlier work on this queue.
    VkMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    dfn.vkCmdPipelineBarrier(command_buffer,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    VkBufferImageCopy region = {};
    region.bufferOffset = VkDeviceSize(fg.width) * fg.height * 4 * 3;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {fg.dump_motion_width, fg.dump_motion_height, 1};
    dfn.vkCmdCopyImageToBuffer(command_buffer, motion.image, VK_IMAGE_LAYOUT_GENERAL, fg.dump_buffer, 1,
                               &region);
  }
  // Back for the effects (fragment, compute for a temporal upscaler), and the copies for the host.
  for (VkImageMemoryBarrier& barrier : barriers) {
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  VkBufferMemoryBarrier buffer_barrier = {};
  buffer_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  buffer_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  buffer_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  buffer_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  buffer_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  buffer_barrier.buffer = fg.dump_buffer;
  buffer_barrier.size = VK_WHOLE_SIZE;
  dfn.vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_HOST_BIT,
                           0, 0, nullptr, 1, &buffer_barrier, 3, barriers);
  fg.dump_pending = true;
  fg.dump_submission = paint_context_.submission_tracker.GetCurrentSubmission();
}

void VulkanPresenter::FrameGenRecordHistory(VkCommandBuffer command_buffer,
                                            VkDescriptorSet guest_output_set, uint64_t sequence) {
  const VulkanDevice::Functions& dfn = vulkan_device_->functions();
  FrameGen& fg = frame_gen_;
  if (sequence == fg.history_sequence) {
    return;  // already kept (a repaint of the same frame)
  }
  // Its pyramid, if the frame in the middle did not make it: in the one not holding the history.
  if (fg.luma_sequence[0] != sequence && fg.luma_sequence[1] != sequence) {
    const uint32_t pyramid = fg.luma_sequence[0] == fg.history_sequence && fg.history_sequence ? 1 : 0;
    FrameGenRecordLuma(command_buffer, guest_output_set, pyramid);
    fg.luma_sequence[pyramid] = sequence;
  }
  // (Set 1 is unused; any but the interpolation's, which holds the history.)
  FrameGenPass(dfn, command_buffer, fg.color_pass, fg.history.framebuffer, fg.width, fg.height,
               fg.copy_pipeline, fg.pipeline_layout, guest_output_set, fg.luma_sets[0][0], nullptr,
               0);
  fg.history_sequence = sequence;
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
