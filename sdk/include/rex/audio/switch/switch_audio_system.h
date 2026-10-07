/**
 * @file        rex/audio/switch/switch_audio_system.h
 * @brief       Audio output for Nintendo Switch through libnx audout
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <memory>
#include <mutex>
#include <queue>
#include <stack>
#include <vector>

#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/downmix.h>
#include <rex/thread.h>

namespace rex::audio::nx {

// One guest render client. SubmitFrame copies the guest frame and returns;
// the output thread of SwitchAudioSystem consumes it.
class SwitchAudioDriver final : public AudioDriver {
 public:
  static constexpr uint32_t kChannels = 6;
  static constexpr uint32_t kChannelSamples = 256;
  static constexpr uint32_t kFrameSamples = kChannels * kChannelSamples;

  SwitchAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SwitchAudioDriver() override;

  void SubmitFrame(uint32_t frame_ptr) override;

  // Output thread only. Folds the oldest queued frame to stereo and adds it to
  // stereo_out (kChannelSamples interleaved pairs), then releases the guest
  // semaphore for it if release is set (sin audio_switch_bomba). Returns false,
  // touching nothing, if no frame is queued.
  bool MixFrameInto(float* stereo_out, const StereoFold& fold, float gain, bool release);

  // Output thread, with audio_switch_bomba: frames delivered by the game, not yet mixed, and one more
  // frame request (a semaphore release).
  size_t QueuedFrames();
  void RequestFrame();

 private:
  rex::thread::Semaphore* semaphore_;
  std::mutex frames_mutex_;
  std::queue<float*> frames_queued_;
  std::stack<float*> frames_unused_;
};

// audout accepts a single 48 kHz stereo s16 stream per process, while the
// guest may register several render clients. The system therefore owns the
// device and one output thread that mixes every client.
class SwitchAudioSystem final : public AudioSystem {
 public:
  explicit SwitchAudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~SwitchAudioSystem() override;

  static bool IsAvailable() { return true; }
  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  void DestroyDriver(AudioDriver* driver) override;

 private:
  // Holds libnx types, which stay out of this header: switch.h declares global
  // Thread, Mutex and Event types.
  struct Output;

  bool StartOutput();
  void StopOutput();
  void OutputThreadMain();
  void FillAndAppend(size_t buffer_index);
  void PumpTick();

  std::unique_ptr<Output> output_;
  // Guards drivers_. Held while mixing, so DestroyDriver never races a mix.
  std::mutex drivers_mutex_;
  std::vector<SwitchAudioDriver*> drivers_;
};

}  // namespace rex::audio::nx
