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

#include <rex/audio/audio_driver.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_ANDROID
#include <sys/resource.h>
#include <cerrno>
#include <cstring>
#endif

namespace rex::audio {

AudioDriver::AudioDriver(memory::Memory* memory) : memory_(memory) {}

AudioDriver::~AudioDriver() = default;

MedidaProduccionAudio g_medida_produccion_audio;

void PrioridadDeAudio([[maybe_unused]] const char* hilo) {
#if REX_PLATFORM_ANDROID
  // On Linux the "process" priority of who = 0 is the calling thread's. Android lets apps go down to -20.
  if (setpriority(PRIO_PROCESS, 0, -16) == 0) {
    REXAPU_INFO("[audio] {}: prioridad de audio (nice -16)", hilo);
  } else {
    REXAPU_WARN("[audio] {}: sin prioridad de audio ({})", hilo, std::strerror(errno));
  }
#endif
}

}  // namespace rex::audio
