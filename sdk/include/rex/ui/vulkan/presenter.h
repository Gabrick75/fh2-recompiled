#pragma once
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

#include <algorithm>
#include <array>
#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rex/assert.h>
#include <rex/ui/presenter.h>
#include <rex/ui/surface.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/instance.h>
#include <rex/ui/vulkan/submission_tracker.h>
#include <rex/ui/vulkan/ui_samplers.h>

namespace rex {
namespace ui {
namespace vulkan {

class VulkanUIDrawContext final : public UIDrawContext {
 public:
  VulkanUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                      uint32_t render_target_height, VkCommandBuffer draw_command_buffer,
                      uint64_t submission_index_current, uint64_t submission_index_completed,
                      VkRenderPass render_pass, VkFormat render_pass_format)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        draw_command_buffer_(draw_command_buffer),
        submission_index_current_(submission_index_current),
        submission_index_completed_(submission_index_completed),
        render_pass_(render_pass),
        render_pass_format_(render_pass_format) {}

  VkCommandBuffer draw_command_buffer() const { return draw_command_buffer_; }
  uint64_t submission_index_current() const { return submission_index_current_; }
  uint64_t submission_index_completed() const { return submission_index_completed_; }
  VkRenderPass render_pass() const { return render_pass_; }
  VkFormat render_pass_format() const { return render_pass_format_; }

 private:
  VkCommandBuffer draw_command_buffer_;
  uint64_t submission_index_current_;
  uint64_t submission_index_completed_;
  // Has 1 subpass with a single render_pass_format_ attachment.
  VkRenderPass render_pass_;
  VkFormat render_pass_format_;
};

// From the latest swapchain: whether its surface offers HDR, and whether it is
// in use (present_hdr).
struct HdrOutputState {
  bool offered = false;
  bool active = false;
  // Active with present_hdr_highlights above 0: the guest output's alpha
  // should say where the game's HUD is (1) and where the scene is (0), so that
  // only the scene is expanded. The refresher leaves it opaque otherwise.
  bool hud_mask = false;
};
HdrOutputState GetHdrOutputState();

class VulkanPresenter final : public Presenter {
 public:
  // Maximum number of different guest output image versions still potentially
  // considered alive that may be given to the refresher - this many instances
  // of dependent objects (such as framebuffers) may need to be kept by the
  // refresher across invocations (due to multiple-buffering of guest output
  // images inside the presenter, different versions may be given even every
  // invocation), to avoid recreation of dependent objects every frame.
  static constexpr size_t kMaxActiveGuestOutputImageVersions = kGuestOutputMailboxSize;

  static constexpr VkFormat kGuestOutputFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  // The guest output is expected to be acquired and released in this state by
  // the refresher. The exception is the first write to the current guest output
  // image - in this case, a barrier is only needed from
  // VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT without access. Also if the image is
  // being refreshed for the first time, it's in VK_IMAGE_LAYOUT_UNDEFINED (but
  // it's safe, and preferred, to transition it from VK_IMAGE_LAYOUT_UNDEFINED
  // when writing to it in general).
  static constexpr VkPipelineStageFlagBits kGuestOutputInternalStageMask =
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  static constexpr VkAccessFlags kGuestOutputInternalAccessMask = VK_ACCESS_SHADER_READ_BIT;
  static constexpr VkImageLayout kGuestOutputInternalLayout =
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  /*
   * present_frame_gen: the game's own motion for a guest frame, when the refresher has it (FH2's velocity pass,
   * nfsmw_nativo_destinos.cpp). The image is in GENERAL layout and written on queue 0 before the refresh's
   * work, so a later paint can sample it; the refresher keeps it alive for a few frames.
   */
  struct FrameGenMotion {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    // Changes whenever `image` is a new image (descriptors are rewritten on a change).
    uint64_t version = 0;
    // The game's motionBlurParams1 that frame, to undo the blur curve the velocity is stored with.
    float params[4] = {};
    bool valid() const { return view != VK_NULL_HANDLE; }

    /*
     * The velocity pass's own inputs, for computing the motion the way it does but without the blur (frame_gen_motion):
     * its depth texture (the scene's depth resolved as 8_8_8_8 with the stencil, GENERAL layout) and its pixel
     * constants c32-c255 (invCurrentVPThenPrevVP, four rows per object id), then c124 (QAndNegNearQAndScale) and the
     * texture's exponent adjust as the last row's x.
     */
    static constexpr uint32_t kConstantRows = 224 + 2;
    VkImage depth_image = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    uint32_t depth_width = 0;
    uint32_t depth_height = 0;
    uint64_t depth_version = 0;
    std::array<float, kConstantRows * 4> constants{};
    bool has_depth() const { return depth_view != VK_NULL_HANDLE; }
  };

  // The callback must use the graphics and compute queue 0 of the device.
  class VulkanGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    VulkanGuestOutputRefreshContext(bool& is_8bpc_out_ref, VkImage image, VkImageView image_view,
                                    uint64_t image_version, bool image_ever_written_previously)
        : GuestOutputRefreshContext(is_8bpc_out_ref),
          image_(image),
          image_view_(image_view),
          image_version_(image_version),
          image_ever_written_previously_(image_ever_written_previously) {}

    // The format is kGuestOutputFormat.
    // Supports usage as a color attachment and as a sampled image, as well as
    // transfer source (but the reason of that is guest output capturing).
    VkImage image() const { return image_; }
    VkImageView image_view() const { return image_view_; }
    uint64_t image_version() const { return image_version_; }
    // Whether a proper barrier must be done to acquire the image.
    bool image_ever_written_previously() const { return image_ever_written_previously_; }
    // present_frame_gen: the game's motion for this frame, if the refresher has it.
    void SetFrameGenMotion(const FrameGenMotion& motion) { motion_ = motion; }
    const FrameGenMotion& frame_gen_motion() const { return motion_; }

   private:
    VkImage image_;
    VkImageView image_view_;
    uint64_t image_version_;
    bool image_ever_written_previously_;
    FrameGenMotion motion_;
  };

  static std::unique_ptr<VulkanPresenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                 const VulkanDevice* vulkan_device,
                                                 const UISamplers* ui_samplers) {
    auto presenter = std::unique_ptr<VulkanPresenter>(
        new VulkanPresenter(host_gpu_loss_callback, vulkan_device, ui_samplers));
    if (!presenter->InitializeSurfaceIndependent()) {
      return nullptr;
    }
    return presenter;
  }

  ~VulkanPresenter();

  const VulkanDevice* vulkan_device() const { return vulkan_device_; }

  static Surface::TypeFlags GetSurfaceTypesSupportedByInstance(
      const VulkanInstance::Extensions& instance_extensions);
  Surface::TypeFlags GetSupportedSurfaceTypes() const override;

  bool CaptureGuestOutput(RawImage& image_out) override;

  void AwaitUISubmissionCompletionFromUIThread(uint64_t submission_index) {
    ui_submission_tracker_.AwaitSubmissionCompletion(submission_index);
  }
  VkCommandBuffer AcquireUISetupCommandBufferFromUIThread();

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;

  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;

  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  // One line with the surface as the driver sees it, at the start of a reconnection loop episode
  // (Presenter::BucleReconexionEmpieza): which call said outdated, with what result, and the swapchain
  // against the surface capabilities, to tell a size or transform mismatch from a lost window.
  void RegistrarSuperficieDesactualizada(const char* llamada, VkResult resultado);

  /*
   * On a reconnection, new acquisition and presentation semaphores for every paint submission.
   *
   * They used to be created once and survive every swapchain. When the driver's
   * QueueSignalReleaseImageANDROID fails (Mali-G615, usuario-20 and usuario-21), Android drops the image
   * without consuming the present semaphore's wait, so the semaphore stays signaled and the next frame
   * signals it again: invalid usage, after which every swapchain failed the same way and the loop never
   * ended. Called with the old swapchain already destroyed and every paint submission completed; waits
   * for the presentation queue so no present still waits on them.
   */
  void RecrearSemaforosDePresentacion();
  uint32_t semaforos_recreados_ = 0;

  /*
   * The queue of paint_context_.present_queue_family that vkQueuePresentKHR goes to: the second one when
   * the family has it and present_segunda_cola is on, queue 0 (the paint's) otherwise or once a
   * reconnection loop moved presentation there (cola_unica_por_bucle_).
   */
  uint32_t ColaDePresentacion() const;
  // Set once by the UI thread (RecrearSemaforosDePresentacion, with the second queue idle) when an episode
  // reaches kCiclosParaColaUnica outdated results there; read by whichever thread paints.
  std::atomic<bool> cola_unica_por_bucle_{false};
  // A resize or rotation takes 1-2; the loop that never ends takes ~24 a second.
  static constexpr uint32_t kCiclosParaColaUnica = 8;

  // Usable for both the guest output image itself and for intermediate images.
  class GuestOutputImage {
   public:
    static std::unique_ptr<GuestOutputImage> Create(const VulkanDevice* const vulkan_device,
                                                    const uint32_t width, const uint32_t height) {
      assert_not_zero(width);
      assert_not_zero(height);
      auto image =
          std::unique_ptr<GuestOutputImage>(new GuestOutputImage(vulkan_device, width, height));
      if (!image->Initialize()) {
        return nullptr;
      }
      return std::move(image);
    }

    GuestOutputImage(const GuestOutputImage& image) = delete;
    GuestOutputImage& operator=(const GuestOutputImage& image) = delete;
    ~GuestOutputImage();

    const VkExtent2D& extent() const { return extent_; }

    VkImage image() const { return image_; }
    VkDeviceMemory memory() const { return memory_; }
    VkImageView view() const { return view_; }

   private:
    GuestOutputImage(const VulkanDevice* const vulkan_device, const uint32_t width,
                     const uint32_t height)
        : vulkan_device_(vulkan_device) {
      extent_.width = width;
      extent_.height = height;
    }

    bool Initialize();

    const VulkanDevice* vulkan_device_;

    VkExtent2D extent_;
    VkImage image_ = VK_NULL_HANDLE;
    VkDeviceMemory memory_ = VK_NULL_HANDLE;
    VkImageView view_ = VK_NULL_HANDLE;
  };

  struct GuestOutputImageInstance {
    // Refresher-side reference (painting has its own references for the purpose
    // of destruction only after painting is done on the GPU).
    std::shared_ptr<GuestOutputImage> image;
    uint64_t version = UINT64_MAX;
    uint64_t last_refresher_submission = 0;
    // For choosing the barrier stage and access mask and layout depending on
    // whether the image has previously been written. If an image is active
    // after a refresh, it can be assumed that this is true.
    bool ever_successfully_refreshed = false;
    // present_frame_gen: the game's motion the last refresh attached (VulkanGuestOutputRefreshContext).
    FrameGenMotion motion;

    void SetToNewImage(const std::shared_ptr<GuestOutputImage>& new_image, uint64_t new_version) {
      image = new_image;
      version = new_version;
      last_refresher_submission = 0;
      ever_successfully_refreshed = false;
    }
  };

  struct GuestOutputPaintRectangleConstants {
    union {
      struct {
        float x;
        float y;
      };
      float offset[2];
    };
    union {
      struct {
        float width;
        float height;
      };
      float size[2];
    };
  };

  enum GuestOutputPaintPipelineLayoutIndex : size_t {
    kGuestOutputPaintPipelineLayoutIndexBilinear,
#if defined(REX_HAS_FIDELITYFX_SDK)
    kGuestOutputPaintPipelineLayoutIndexCasSharpen,
    kGuestOutputPaintPipelineLayoutIndexCasResample,
    kGuestOutputPaintPipelineLayoutIndexFsrEasu,
    kGuestOutputPaintPipelineLayoutIndexFsrRcas,
#endif
    // present_hdr's last pass (HdrOutputConstants).
    kGuestOutputPaintPipelineLayoutIndexHdr,

    kGuestOutputPaintPipelineLayoutCount,
  };

  static constexpr GuestOutputPaintPipelineLayoutIndex GetGuestOutputPaintPipelineLayoutIndex(
      GuestOutputPaintEffect effect) {
    switch (effect) {
      case GuestOutputPaintEffect::kBilinear:
      case GuestOutputPaintEffect::kBilinearDither:
        return kGuestOutputPaintPipelineLayoutIndexBilinear;
#if defined(REX_HAS_FIDELITYFX_SDK)
      case GuestOutputPaintEffect::kCasSharpen:
      case GuestOutputPaintEffect::kCasSharpenDither:
        return kGuestOutputPaintPipelineLayoutIndexCasSharpen;
      case GuestOutputPaintEffect::kCasResample:
      case GuestOutputPaintEffect::kCasResampleDither:
        return kGuestOutputPaintPipelineLayoutIndexCasResample;
      case GuestOutputPaintEffect::kFsrEasu:
        return kGuestOutputPaintPipelineLayoutIndexFsrEasu;
      case GuestOutputPaintEffect::kFsrRcas:
      case GuestOutputPaintEffect::kFsrRcasDither:
        return kGuestOutputPaintPipelineLayoutIndexFsrRcas;
#endif
      default:
        assert_unhandled_case(effect);
        return kGuestOutputPaintPipelineLayoutCount;
    }
  }

  struct PaintContext {
    class Submission {
     public:
      static std::unique_ptr<Submission> Create(const VulkanDevice* const vulkan_device) {
        auto submission = std::unique_ptr<Submission>(new Submission(vulkan_device));
        if (!submission->Initialize()) {
          return nullptr;
        }
        return submission;
      }

      Submission(const Submission& submission) = delete;
      Submission& operator=(const Submission& submission) = delete;
      ~Submission();

      VkSemaphore acquire_semaphore() const { return acquire_semaphore_; }
      // present_esperar_adquisicion_en_cpu (vulkan_presenter.cpp).
      VkFence acquire_fence() const { return acquire_fence_; }
      VkSemaphore present_semaphore() const { return present_semaphore_; }
      VkCommandPool draw_command_pool() const { return draw_command_pool_; }
      VkCommandBuffer draw_command_buffer() const { return draw_command_buffer_; }

      // Replaces both semaphores with new ones. Only when nothing that uses them is in flight
      // (RecrearSemaforosDePresentacion).
      bool RecrearSemaforos();

     private:
      explicit Submission(const VulkanDevice* const vulkan_device)
          : vulkan_device_(vulkan_device) {}
      bool Initialize();

      const VulkanDevice* vulkan_device_;
      VkSemaphore acquire_semaphore_ = VK_NULL_HANDLE;
      VkFence acquire_fence_ = VK_NULL_HANDLE;
      VkSemaphore present_semaphore_ = VK_NULL_HANDLE;
      VkCommandPool draw_command_pool_ = VK_NULL_HANDLE;
      VkCommandBuffer draw_command_buffer_ = VK_NULL_HANDLE;
    };

    /*
     * Four frames in flight instead of three (tried and reverted, see below).
     *
     * Measured on the console with a 33 ms configuration: GPU work already fits (33.35 ms real), and
     * so does the ring CPU (27.3 ms of work), and yet the frame takes 38.5. The difference is 5.65 ms of
     * stalled GPU, because the thread that feeds it spends 11.7 ms per frame blocked in presentation,
     * and not waiting for the GPU: our own wait counter shows 0.03 ms per Swap.
     *
     * Part of that blocking is AwaitSubmissionCompletion on entering PaintAndPresentImpl, which waits
     * for the paint from kSubmissionCount frames ago to finish. With 4 instead of 3 there is one more
     * frame of margin before having to wait for anyone.
     *
     * It costs one more command pool, semaphore and fence, and one more image in the swapchain
     * (minImageCount is tied to this).
     */
    /*
     * Tried and reverted. With 4 the stalled-GPU gap went from 5.65 to 5.85 ms: it did not help, and
     * each frame in flight adds ~40 ms of input latency. The presentation blocking was not
     * AwaitSubmissionCompletion. The extra swapchain image (minImageCount = kSubmissionCount + 1) stays.
     */
    static constexpr uint32_t kSubmissionCount = 3;

    struct GuestOutputPaintPipeline {
      // Created on initialization.
      VkPipeline intermediate_pipeline = VK_NULL_HANDLE;
      // Created during guest output painting (after awaiting the last guest
      // output paint if outdated and needs to be recreated), when needed, for
      // the up-to-date render pass that draws to the swapchain with the actual
      // image format.
      VkPipeline swapchain_pipeline = VK_NULL_HANDLE;
      VkFormat swapchain_format = VK_FORMAT_UNDEFINED;
    };

    // The mailbox images, then present_frame_gen's frame in the middle in a slot of its own, so it never
    // pushes a mailbox image out of the references (that would rewrite a descriptor every frame).
    static constexpr uint32_t kGuestOutputPaintRefFrameGen = kGuestOutputMailboxSize;
    static constexpr uint32_t kGuestOutputPaintRefCount = kGuestOutputMailboxSize + 1;

    enum GuestOutputDescriptorSet : uint32_t {
      kGuestOutputDescriptorSetGuestOutput0Sampled,

      kGuestOutputDescriptorSetIntermediate0Sampled =
          kGuestOutputDescriptorSetGuestOutput0Sampled + kGuestOutputPaintRefCount,

      kGuestOutputDescriptorSetCount =
          kGuestOutputDescriptorSetIntermediate0Sampled + kMaxGuestOutputPaintEffects - 1,
    };

    struct UISetupCommandBuffer {
      UISetupCommandBuffer(VkCommandPool command_pool, VkCommandBuffer command_buffer,
                           uint64_t last_usage_submission_index = 0)
          : command_pool(command_pool),
            command_buffer(command_buffer),
            last_usage_submission_index(last_usage_submission_index) {}

      VkCommandPool command_pool;
      VkCommandBuffer command_buffer;
      uint64_t last_usage_submission_index;
    };

    struct SwapchainFramebuffer {
      SwapchainFramebuffer(VkImageView image_view, VkFramebuffer framebuffer)
          : image_view(image_view), framebuffer(framebuffer) {}

      VkImageView image_view;
      VkFramebuffer framebuffer;
    };

    /*
     * present_hdr (from arcanite24/shiftglue-sdk, Pinyon Shift's HDR output). Everything above paints
     * into an SDR stand-in per swapchain image as it would into an SDR swapchain; one last pass decodes
     * it, scales white to the paper white and writes the HDR swapchain (HDR10 on Android).
     */
    struct HdrImage {
      VkImage image = VK_NULL_HANDLE;
      VkDeviceMemory memory = VK_NULL_HANDLE;
      // Sampled by the last pass, which decodes the sRGB.
      VkImageView srgb_view = VK_NULL_HANDLE;
      VkImageView swapchain_view = VK_NULL_HANDLE;
      VkFramebuffer swapchain_framebuffer = VK_NULL_HANDLE;
      VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    };
    // scRGB, or HDR10 where the surface pairs it with BT.2020 PQ.
    static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    // HDR10 where the surface offers no float format with it (Android).
    static constexpr VkFormat kHdrPqFormat = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    static constexpr VkFormat kHdrStandInFormat = VK_FORMAT_B8G8R8A8_UNORM;
    static constexpr VkFormat kHdrStandInSrgbFormat = VK_FORMAT_B8G8R8A8_SRGB;

    explicit PaintContext(const VulkanDevice* const vulkan_device)
        : vulkan_device(vulkan_device), submission_tracker(vulkan_device) {}
    PaintContext(const PaintContext& paint_context) = delete;
    PaintContext& operator=(const PaintContext& paint_context) = delete;

    // The old swapchain, if passed, should be assumed to be retired after this
    // call (though it may fail before the vkCreateSwapchainKHR that will
    // technically retire it, so it will be in an undefined state), and needs to
    // be destroyed externally no matter what the result is.
    static VkSwapchainKHR CreateSwapchainForVulkanSurface(
        const VulkanDevice* vulkan_device, VkSurfaceKHR surface, uint32_t width, uint32_t height,
        VkSwapchainKHR old_swapchain, uint32_t& present_queue_family_out,
        VkFormat& image_format_out, VkExtent2D& image_extent_out, bool& is_fifo_out,
        bool& hdr_out, bool& ui_surface_unusable_out, bool registrar);

    // Destroys the swapchain and its derivatives, nulls `swapchain` and returns
    // the original swapchain object, if it existed, for use as oldSwapchain if
    // needed and for destruction.
    VkSwapchainKHR PrepareForSwapchainRetirement();
    // May be called from the destructor of the presenter.
    void DestroySwapchainAndVulkanSurface();

    // Connection-indepedent.

    const VulkanDevice* vulkan_device;

    std::array<std::unique_ptr<PaintContext::Submission>, kSubmissionCount> submissions;
    VulkanSubmissionTracker submission_tracker;

    /*
     * Early acquisition, without ever blocking.
     *
     * Measured on the console: GPU work already fits in 33.3 ms (33.03), but the frame takes 38.8
     * because the GPU spends 6.73 ms stalled. And it is not waiting for the CPU to finish recording (our
     * counter of ring waits for the GPU shows 0.03 ms per Swap): it is waiting for the thread to leave
     * presentation, where it stays 11.7 ms per frame.
     *
     * That blocking is vkAcquireNextImageKHR with an infinite timeout, called at the end of the frame.
     *
     * How it works. The image for the next frame is requested right after presenting the current one,
     * with a zero timeout. If one is free it is kept and the coming frame waits for nothing; if not,
     * nothing happens: things continue as usual and the image is requested with a wait at the start of
     * the next frame. So the early acquisition can never block or make the bad case worse.
     *
     * Why dedicated semaphores are needed. Each Submission's semaphore is waited on by its frame's
     * submit, and that submit may still be in flight: reusing it would be a validation error. With
     * kSubmissionCount + 1 and rotation, the semaphore in use was last used kSubmissionCount + 1 frames
     * ago, and AwaitSubmissionCompletion has already guaranteed completion up to kSubmissionCount back.
     */
    std::array<VkSemaphore, kSubmissionCount + 1> semaforos_anticipados{};
    uint32_t anticipado_siguiente = 0;
    uint32_t imagen_anticipada = UINT32_MAX;
    VkSemaphore semaforo_anticipado = VK_NULL_HANDLE;
    bool anticipada_valida = false;
    uint64_t anticipadas_aciertos = 0;
    uint64_t anticipadas_fallos = 0;

    std::array<GuestOutputPaintPipeline, size_t(GuestOutputPaintEffect::kCount)>
        guest_output_paint_pipelines;

    VkDescriptorPool guest_output_descriptor_pool = VK_NULL_HANDLE;
    // Descriptors are updated while painting if they're out of date.
    VkDescriptorSet guest_output_descriptor_sets[kGuestOutputDescriptorSetCount];

    // Refreshed and cleaned up during guest output painting. The first is the
    // paint submission index in which the guest output image (and its
    // descriptors) was last used, the second is the reference to the image,
    // which may be null. The indices are not mailbox indices here, rather, if
    // the reference is not in this array yet, the most outdated reference, if
    // needed, is replaced with the new one, awaiting the usage completion of
    // the last paint usage.
    std::array<std::pair<uint64_t, std::shared_ptr<GuestOutputImage>>, kGuestOutputPaintRefCount>
        guest_output_image_paint_refs;
    // The latest submission index at which any guest output image was drawn.
    uint64_t guest_output_image_paint_last_submission = 0;

    // Current intermediate images for guest output painting, refreshed when
    // painting guest output.
    std::array<std::unique_ptr<GuestOutputImage>, kMaxGuestOutputPaintEffects - 1>
        guest_output_intermediate_images;
    // Created and destroyed alongside the images. UNORM only.
    std::array<VkFramebuffer, kMaxGuestOutputPaintEffects - 1>
        guest_output_intermediate_framebuffers = {};
    uint64_t guest_output_intermediate_image_last_submission = 0;

    // Command buffers optionally executed before the draw command buffer,
    // outside the painting render pass.
    std::vector<UISetupCommandBuffer> ui_setup_command_buffers;
    size_t ui_setup_command_buffer_current_index = SIZE_MAX;

    // Connection-specific.

    // May be reused between connections if the format stays the same.
    VkRenderPass swapchain_render_pass = VK_NULL_HANDLE;
    VkFormat swapchain_render_pass_format = VK_FORMAT_UNDEFINED;
    bool swapchain_render_pass_clear_load_op = false;
    // Whether swapchain_render_pass ends in an HDR stand-in to be sampled.
    bool swapchain_render_pass_hdr = false;
    // The last pass into the HDR swapchain, and its pipeline, for a swapchain
    // of hdr_render_pass_format.
    VkRenderPass hdr_render_pass = VK_NULL_HANDLE;
    VkFormat hdr_render_pass_format = VK_FORMAT_UNDEFINED;
    VkPipeline hdr_pipeline = VK_NULL_HANDLE;
    // Replaces the alpha of the painted guest output with the guest output's
    // HUD coverage, in swapchain_render_pass (always kHdrStandInFormat).
    VkPipeline hdr_ui_mask_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool hdr_descriptor_pool = VK_NULL_HANDLE;

    VkSurfaceKHR vulkan_surface = VK_NULL_HANDLE;
    // The ANativeWindow of vulkan_surface on Android, for its data space.
    void* android_window = nullptr;
    uint32_t present_queue_family = UINT32_MAX;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    // The size everything but the HDR pass paints at; an HDR swapchain pre-rotated for the panel has
    // it swapped (HdrSwapchainImageExtent).
    VkExtent2D swapchain_extent = {};
    bool swapchain_is_fifo = false;
    std::vector<VkImage> swapchain_images;
    std::vector<SwapchainFramebuffer> swapchain_framebuffers;
    bool swapchain_hdr = false;
    // Per swapchain image when swapchain_hdr.
    std::vector<HdrImage> hdr_images;
  };

  explicit VulkanPresenter(HostGpuLossCallback host_gpu_loss_callback,
                           const VulkanDevice* vulkan_device, const UISamplers* ui_samplers)
      : Presenter(host_gpu_loss_callback),
        vulkan_device_(vulkan_device),
        ui_samplers_(ui_samplers),
        guest_output_image_refresher_submission_tracker_(vulkan_device),
        ui_submission_tracker_(vulkan_device),
        paint_context_(vulkan_device) {
    assert_not_null(vulkan_device);
    assert_not_null(ui_samplers);
  }

  bool InitializeSurfaceIndependent();

  /*
   * Seals with an empty submission everything the refresh has sent to the queue since last time and
   * waits for the GPU to reach that point. This used to be done once per frame at the end of
   * RefreshGuestOutputImpl; now it is only called where it is really needed: when the guest image has
   * to be destroyed (frontbuffer size change) and at shutdown. It does nothing if there has been no
   * refresh since the last seal. See present_refresco_sellado_perezoso in vulkan_presenter.cpp.
   */
  void SellarRefrescosYEsperar();

  // fragment_shader replaces the effect's own, with the effect's layout unless
  // layout is given.
  [[nodiscard]] VkPipeline CreateGuestOutputPaintPipeline(
      GuestOutputPaintEffect effect, VkRenderPass render_pass,
      VkShaderModule fragment_shader = VK_NULL_HANDLE, VkPipelineLayout layout = VK_NULL_HANDLE,
      VkColorComponentFlags color_write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
  bool CreateHdrSwapchainImages(VkFormat swapchain_format);

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  bool EnsureTemporalUpscalerContext(uint32_t render_width, uint32_t render_height,
                                     uint32_t output_width, uint32_t output_height);
  bool DispatchTemporalUpscaler(VkCommandBuffer command_buffer, VkImage input_image,
                                uint32_t input_width, uint32_t input_height, VkImage output_image,
                                uint32_t output_width, uint32_t output_height,
                                const GuestOutputPaintConfig& config);
  void DestroyTemporalUpscalerContext();
#endif

  /*
   * present_frame_gen (vulkan_presenter_frame_gen.cpp): a frame in the middle of every two guest
   * frames, from the images alone, painted through the same effects as a real one.
   *
   * Everything is recorded into the paint command buffer, as fragment passes into formats every device
   * can render to and filter (the guest output's 10:10:10:2 is not a storage format everywhere):
   *   - a luma pyramid of each frame at 1/4, 1/8, 1/16 and 1/32 of the guest output (two, the previous
   *     frame's and the current one's);
   *   - a symmetric block search: every texel at 1/32, then refined at 1/16, 1/8 and 1/4, and a 3x3
   *     vector median of the last level;
   *   - the frame in the middle at the guest output's size, into `output`;
   *   - after the real frame is painted, a copy of it into `history`, because the mailbox reuses its
   *     images.
   * Accessed only while painting (paint_mode_mutex_ held) and in the destructor.
   */
  struct FrameGenTarget {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
  };
  // 1/4, 1/8, 1/16 and 1/32 of the guest output.
  static constexpr uint32_t kFrameGenLevels = 4;
  struct FrameGen {
    bool initialized = false;
    bool failed = false;
    VkRenderPass luma_pass = VK_NULL_HANDLE;    // R8_UNORM
    VkRenderPass vector_pass = VK_NULL_HANDLE;  // R16G16_SFLOAT
    VkRenderPass color_pass = VK_NULL_HANDLE;   // kGuestOutputFormat
    // Set 0 is the guest output's own paint descriptor set; set 1 is one of these.
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule luma_fs = VK_NULL_HANDLE;
    VkShaderModule search_fs = VK_NULL_HANDLE;
    VkShaderModule interpolate_fs = VK_NULL_HANDLE;
    VkShaderModule copy_fs = VK_NULL_HANDLE;
    VkShaderModule median_fs = VK_NULL_HANDLE;
    VkPipeline luma_pipeline = VK_NULL_HANDLE;
    VkPipeline search_pipeline = VK_NULL_HANDLE;
    VkPipeline interpolate_pipeline = VK_NULL_HANDLE;
    VkPipeline copy_pipeline = VK_NULL_HANDLE;
    VkPipeline median_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

    // The guest output size the targets below are for; 0 before the first frame.
    uint32_t width = 0;
    uint32_t height = 0;
    FrameGenTarget history;
    FrameGenTarget luma[2][kFrameGenLevels];
    // The vectors of each level, in guest pixels.
    FrameGenTarget vectors[kFrameGenLevels];
    // The finest vectors after the 3x3 vector median, which the interpolation reads.
    FrameGenTarget smoothed;
    std::shared_ptr<GuestOutputImage> output;
    VkFramebuffer output_framebuffer = VK_NULL_HANDLE;
    // [pyramid][level]: the level above as the source (level 0 reads the guest output from set 0).
    VkDescriptorSet luma_sets[2][kFrameGenLevels] = {};
    // [current pyramid][level]: both pyramids' level and the vectors of the level above.
    VkDescriptorSet search_sets[2][kFrameGenLevels] = {};
    VkDescriptorSet interpolate_set = VK_NULL_HANDLE;
    VkDescriptorSet median_set = VK_NULL_HANDLE;
    // Set 2: the game's velocity (FrameGenMotion), one per slot of the refresher's rotation (version % 4, the
    // set rewritten when that slot's image changes), and the last one for frames without it.
    static constexpr uint32_t kMotionSlots = 4;
    VkDescriptorSetLayout motion_set_layout = VK_NULL_HANDLE;
    VkDescriptorSet motion_sets[kMotionSlots + 1] = {};
    VkImageView motion_set_views[kMotionSlots] = {};
    // Mode 2 (frame_gen_motion.ps.glsl): the motion computed from the velocity pass's depth and matrices, at half
    // the guest output's size, read through `game_motion_set` (set 2 of the search and the interpolation). Its own
    // set 2 per slot of the refresher's rotation: the depth texture and a uniform buffer with the constants.
    FrameGenTarget game_motion;
    VkDescriptorSet game_motion_set = VK_NULL_HANDLE;
    VkDescriptorSetLayout depth_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout motion_pipeline_layout = VK_NULL_HANDLE;
    VkShaderModule motion_fs = VK_NULL_HANDLE;
    VkPipeline motion_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet depth_sets[kMotionSlots] = {};
    VkImageView depth_set_views[kMotionSlots] = {};
    VkBuffer constant_buffers[kMotionSlots] = {};
    VkDeviceMemory constant_memory[kMotionSlots] = {};
    void* constant_mappings[kMotionSlots] = {};
    uint64_t with_depth = 0;

    // The guest output (GuestOutputProperties::sequence) each pyramid and the history hold, 0 = none.
    uint64_t luma_sequence[2] = {};
    uint64_t history_sequence = 0;
    // The guest output whose frame in the middle (before it) `output` holds.
    uint64_t output_sequence = 0;

    // For the log.
    uint64_t intermediates = 0;
    uint64_t skipped = 0;
    uint64_t with_motion = 0;
    // GPU time of each frame in the middle, from the pyramid to the interpolation: two timestamps per slot,
    // a slot read back kTimestampSlots frames in the middle later (by then its paint has completed:
    // PaintAndPresentImpl awaits all but the last kSubmissionCount paints). No pool where the queue has no
    // timestamps.
    static constexpr uint32_t kTimestampSlots = 4;
    VkQueryPool timestamps = VK_NULL_HANDLE;
    float timestamp_period_ns = 0.0f;
    uint32_t timestamp_slot = 0;
    bool timestamp_written[kTimestampSlots] = {};
    double gpu_ms_sum = 0.0;
    uint64_t gpu_ms_count = 0;

    // present_frame_gen_volcar_cada_s: the previous frame, the one in the middle and the current one of the
    // same paint, copied to `dump_memory` and written out once that paint has completed.
    VkBuffer dump_buffer = VK_NULL_HANDLE;
    VkDeviceMemory dump_memory = VK_NULL_HANDLE;
    uint32_t dump_width = 0;
    uint32_t dump_height = 0;
    // The game's velocity copied with the three frames (4 bytes per texel), if the frame had it.
    uint32_t dump_motion_width = 0;
    uint32_t dump_motion_height = 0;
    VkFormat dump_motion_format = VK_FORMAT_UNDEFINED;
    float dump_motion_params[4] = {};
    bool dump_pending = false;
    uint64_t dump_submission = 0;
    uint32_t dumps = 0;
    std::chrono::steady_clock::time_point last_dump{};
  };

  bool FrameGenInitialize();
  void FrameGenShutdown();
  // Recreates the targets when the guest output size changes (initializing first if needed), with the
  // barriers for the new images in `command_buffer`. False if they could not be created.
  bool FrameGenEnsureSize(VkCommandBuffer command_buffer, uint32_t width, uint32_t height);
  void FrameGenDestroyTargets();
  // The luma pyramid of the guest output in `guest_output_set` into pyramid `pyramid`.
  void FrameGenRecordLuma(VkCommandBuffer command_buffer, VkDescriptorSet guest_output_set,
                          uint32_t pyramid);
  // The frame between the history and the guest output `sequence`, into frame_gen_.output. False
  // (and nothing recorded) if there is no previous frame to start from.
  bool FrameGenRecordIntermediate(VkCommandBuffer command_buffer, VkDescriptorSet guest_output_set,
                                  VkImage guest_output_image, const FrameGenMotion& motion,
                                  uint64_t sequence);
  // present_frame_gen_volcar_cada_s: records the copies of the three frames when one is due, and writes
  // out the previous one once its paint has completed.
  void FrameGenDump(VkCommandBuffer command_buffer, VkImage guest_output_image,
                    const FrameGenMotion& motion);
  // Keeps the guest output `sequence` as the history (and its pyramid), once per guest frame.
  void FrameGenRecordHistory(VkCommandBuffer command_buffer, VkDescriptorSet guest_output_set,
                             uint64_t sequence);
  bool IsFrameGenAvailable() const override;
  // The set 2 for this frame's game velocity (the empty one without it).
  VkDescriptorSet FrameGenMotionSet(const FrameGenMotion& motion);
  // For CaptureGuestOutput: the frame in the middle when present_frame_gen_capturar_intermedio asks for it
  // and one has been made, else null.
  std::shared_ptr<GuestOutputImage> FrameGenCaptureImage() const;
  // present_frame_gen_depurar: 0 normal, 1 the real frame in place of the one in the middle (same pace), 2 the
  // one in the middle in place of the real one too.
  int32_t FrameGenDebugMode() const;

  FrameGen frame_gen_;

  const VulkanDevice* vulkan_device_;
  const UISamplers* ui_samplers_;

  // Static objects for guest output presentation, used only when painting the
  // main target (can be destroyed only after awaiting main target usage
  // completion).
  VkDescriptorSetLayout guest_output_paint_image_descriptor_set_layout_ = VK_NULL_HANDLE;
  std::array<VkPipelineLayout, kGuestOutputPaintPipelineLayoutCount>
      guest_output_paint_pipeline_layouts_ = {};
  VkShaderModule guest_output_paint_vs_ = VK_NULL_HANDLE;
  VkShaderModule hdr_output_fs_ = VK_NULL_HANDLE;
  VkShaderModule hdr_ui_mask_fs_ = VK_NULL_HANDLE;
  std::array<VkShaderModule, size_t(GuestOutputPaintEffect::kCount)> guest_output_paint_fs_ = {};
  // Not compatible with the swapchain render pass even if the format is the
  // same due to different dependencies (this is shader read > color
  // attachment > shader read).
  VkRenderPass guest_output_intermediate_render_pass_ = VK_NULL_HANDLE;

  // Value monotonically increased every time a new guest output image is
  // initialized, for recreation of dependent objects such as framebuffers in
  // the refreshers - saving and comparing the handle in the refresher is not
  // enough as Create > Destroy > Create may result in the same handle for
  // actually different objects without the refresher being aware of the
  // destruction.
  uint64_t guest_output_image_next_version_ = 0;
  std::array<GuestOutputImageInstance, kGuestOutputMailboxSize> guest_output_images_;
  VulkanSubmissionTracker guest_output_image_refresher_submission_tracker_;
  /*
   * With present_refresco_sellado_perezoso, instead of sealing with an empty submission on every
   * refresh, this only marks that there are unsealed refresh commands. The refreshing thread (the
   * ring) sets it and the UI thread also reads it in the destructor, hence the atomic; relaxed is
   * enough because the queue submission itself provides the real synchronization.
   */
  std::atomic<bool> refrescos_sin_sellar_{false};

  // UI submission tracker with the submission index that can be given to UI
  // drawers (accessible from the UI thread only, at any time).
  VulkanSubmissionTracker ui_submission_tracker_;

  // Accessible only by painting and by surface connection lifetime management
  // (ConnectOrReconnectPaintingToSurfaceFromUIThread,
  // DisconnectPaintingFromSurfaceFromUIThreadImpl) by the thread doing it, as
  // well as by presenter initialization and shutdown.
  PaintContext paint_context_;

#if defined(REX_HAS_FIDELITYFX_RUNTIME) && REX_HAS_FIDELITYFX_RUNTIME
  void* temporal_upscaler_context_ = nullptr;
  uint32_t temporal_upscaler_max_render_width_ = 0;
  uint32_t temporal_upscaler_max_render_height_ = 0;
  uint32_t temporal_upscaler_max_output_width_ = 0;
  uint32_t temporal_upscaler_max_output_height_ = 0;
  bool temporal_upscaler_provider_logged_ = false;
#endif
};

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
