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

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/kernel.h>
#include <rex/memory.h>

namespace rex::audio {

// Android: the calling thread to THREAD_PRIORITY_AUDIO (nice -16), the level AudioTrack's own threads use, so
// the threads that keep the output fed are not left waiting behind the game's busy threads. Elsewhere nothing.
void PrioridadDeAudio(const char* hilo);

// Where the blocks go between the SDL pump and the output, reported (and reset) with "[audio] SDL en 10 s".
struct MedidaProduccionAudio {
  std::atomic<uint32_t> liberadas{0};      // pump releases the game's semaphore took
  std::atomic<uint32_t> rechazadas{0};     // pump releases refused: the semaphore was already full
  std::atomic<uint64_t> retraso_max_ns{0}; // the pump's worst wake-up past its deadline
  std::atomic<uint32_t> callbacks{0};      // game audio callbacks the Audio Worker ran
  std::atomic<uint64_t> callback_ns{0};
  std::atomic<uint64_t> callback_max_ns{0};
  std::atomic<uint32_t> entregadas{0};     // frames the game submitted (SubmitFrame)
  std::atomic<uint64_t> hueco_max_ns{0};   // the longest time between two submitted frames
};
extern MedidaProduccionAudio g_medida_produccion_audio;

inline void MaximoAtomico(std::atomic<uint64_t>& maximo, uint64_t valor) {
  uint64_t actual = maximo.load(std::memory_order_relaxed);
  while (valor > actual && !maximo.compare_exchange_weak(actual, valor, std::memory_order_relaxed)) {
  }
}

class AudioDriver {
 public:
  explicit AudioDriver(memory::Memory* memory);
  virtual ~AudioDriver();

  virtual void SubmitFrame(uint32_t samples_ptr) = 0;

 protected:
  inline uint8_t* TranslatePhysical(uint32_t guest_address) const {
    return memory_->TranslatePhysical(guest_address);
  }

  memory::Memory* memory_ = nullptr;
};

}  // namespace rex::audio
