#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace rex::audio {

// Linked channels preserve stereo balance. Feed unclipped floats: clipping before
// this stage destroys peaks the limiter needs to measure. State persists across blocks.
inline void LimitOutput(float* data, size_t frames, size_t channels, int sample_rate,
                        float& gain) {
  constexpr float ceiling = 0.97f;
  const float recovery = 1.0f / (0.080f * std::max(sample_rate, 1));
  for (size_t i = 0; i < frames; ++i) {
    float* frame = data + i * channels;
    float peak = 0.0f;
    for (size_t ch = 0; ch < channels; ++ch) {
      if (!std::isfinite(frame[ch])) frame[ch] = 0.0f;
      peak = std::max(peak, std::fabs(frame[ch]));
    }
    const float target = peak > ceiling ? ceiling / peak : 1.0f;
    gain = target < gain ? target : gain + (target - gain) * recovery;
    for (size_t ch = 0; ch < channels; ++ch) {
      frame[ch] = std::clamp(frame[ch] * gain, -ceiling, ceiling);
    }
  }
}

}  // namespace rex::audio
