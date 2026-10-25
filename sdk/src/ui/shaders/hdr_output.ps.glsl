#version 460
#extension GL_EXT_samplerless_texture_functions : require

// From arcanite24/shiftglue-sdk (BSD-3-Clause), the HDR output of Pinyon
// Shift. Compiled with tools/compilar_shaders.sh.
// present_hdr: the SDR stand-in (guest output and UI, sampled through its
// sRGB view, so linear) to the HDR swapchain. The pass works in scRGB, where
// 1.0 is 80 nits, and writes that as is, or encoded as HDR10 (BT.2020 and
// the PQ curve) where the swapchain is HDR10. The
// stand-in's alpha is how much of the pixel is UI: the HUD of the guest output
// and the host UI stay at the paper white, and only the scene's highlights
// are expanded towards the highlight peak.

layout(push_constant) uniform HdrOutputConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) float paper_white_scale;  // Paper white nits / 80.
  float highlight_scale;  // Highlight peak / paper white - 1, 0 for none.
  float knee;             // Brightness where the expansion starts.
  float knee_range_inv;   // 1 / (1 - knee).
  // 0 decodes the stand-in as piecewise sRGB, otherwise as this power, as an
  // SDR display with that gamma would show it.
  float gamma;
  // The brightness the expansion follows: 0 is the luminance, 1 the largest
  // channel, so that saturated lights (red brake lights) expand too.
  float max_channel_weight;
  // Pixels left of it show the SDR image as Windows composes SDR content
  // (piecewise sRGB at the paper white), with a divider; negative for none.
  float compare_x;
  // A calibration pattern under the UI (kPattern*), and its value.
  uint pattern;
  float pattern_value;  // kPatternPeak: the tested peak, nits / 80.
  // Bit 0: an HDR10 swapchain, written as BT.2020 PQ. Bits 1-2: the
  // swapchain is pre-rotated for the display (kRotation*), so its pixels map
  // to rotated stand-in pixels.
  uint output_flags;
  vec2 size;  // The stand-in's (unrotated) size, for the patterns.
  // The expansion follows t^curve from the knee: below 2, light that is
  // bright but not white (FH2's headlights, ~75 % linear) gets most of it.
  float curve;
} constants;

const uint kOutputPq = 1u;
const uint kRotationShift = 1u;
const uint kRotationNone = 0u;
const uint kRotation90 = 1u;
const uint kRotation270 = 2u;

const uint kPatternNone = 0u;
const uint kPatternBlack = 1u;
const uint kPatternPaperWhite = 2u;
const uint kPatternPeak = 3u;

layout(set = 0, binding = 0) uniform texture2D stand_in;

layout(location = 0) out vec4 output_color;

float max3(vec3 v) { return max(v.r, max(v.g, v.b)); }

// scRGB (linear BT.709, 1.0 at 80 nits) as the swapchain stores it.
vec4 Output(vec3 scrgb) {
  if ((constants.output_flags & kOutputPq) == 0u) {
    return vec4(scrgb, 1.0);
  }
  const mat3 kBt709ToBt2020 = mat3(0.6274040, 0.0690970, 0.0163916,
                                   0.3292820, 0.9195400, 0.0880132,
                                   0.0433136, 0.0113612, 0.8955950);
  vec3 y = clamp(kBt709ToBt2020 * scrgb * (80.0 / 10000.0), 0.0, 1.0);
  const float m1 = 2610.0 / 16384.0;
  const float m2 = 2523.0 / 4096.0 * 128.0;
  const float c1 = 3424.0 / 4096.0;
  const float c2 = 2413.0 / 4096.0 * 32.0;
  const float c3 = 2392.0 / 4096.0 * 32.0;
  vec3 p = pow(y, vec3(m1));
  return vec4(pow((c1 + c2 * p) / (1.0 + c3 * p), vec3(m2)), 1.0);
}

vec3 SrgbEncode(vec3 linear) {
  linear = clamp(linear, 0.0, 1.0);
  return mix(1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, linear * 12.92,
             lessThanEqual(linear, vec3(0.0031308)));
}

// An SDR code value as the image's decode shows it, in scRGB.
float SdrLevel(float code) {
  float linear = constants.gamma > 0.0
      ? pow(code, constants.gamma)
      : (code <= 0.04045 ? code / 12.92 : pow((code + 0.055) / 1.055, 2.4));
  return linear * constants.paper_white_scale;
}

bool InRect(vec2 uv, vec2 center, vec2 half_size) {
  return all(lessThan(abs(uv - center), half_size));
}

// The calibration patterns, in scRGB, in a band below the menu's rows that
// the menu leaves undimmed (MenuScreen::set_backdrop_gap); the rest is black.
const float kPatternTop = 0.62;
const float kPatternBottom = 0.88;

vec3 Pattern(vec2 pixel) {
  vec2 uv = pixel / constants.size;
  if (uv.y < kPatternTop || uv.y > kPatternBottom) {
    return vec3(0.0);
  }
  uv.y = (uv.y - kPatternTop) / (kPatternBottom - kPatternTop);
  // Squares in the band's own coordinates.
  vec2 square =
      vec2(constants.size.y * (kPatternBottom - kPatternTop) / constants.size.x, 1.0);
  if (constants.pattern == kPatternBlack) {
    // Black with squares at SDR code values 1 % to 6 %: the TV's brightness
    // is right when the 1 % square is invisible and the 2 % one just shows.
    for (int i = 0; i < 6; ++i) {
      if (InRect(uv, vec2((float(i) + 0.5) / 6.0, 0.5), square * 0.3)) {
        return vec3(SdrLevel(float(i + 1) / 100.0));
      }
    }
    return vec3(0.0);
  }
  if (constants.pattern == kPatternPaperWhite) {
    // A white square beside SDR gray steps on mid gray: white should look
    // like a white sheet of paper in the room, the steps as distinct as on
    // an SDR screen.
    if (InRect(uv, vec2(0.2, 0.5), square * 0.35)) {
      return vec3(SdrLevel(1.0));
    }
    if (uv.y > 0.3 && uv.y < 0.7 && uv.x > 0.4 && uv.x < 0.95) {
      return vec3(SdrLevel(floor((uv.x - 0.4) / 0.55 * 11.0) / 10.0));
    }
    return vec3(SdrLevel(0.5));
  }
  if (constants.pattern == kPatternPeak) {
    // The brightest the swapchain can carry around a square at the tested
    // peak: the square disappears once the tested peak reaches what the
    // display shows.
    if (InRect(uv, vec2(0.5, 0.5), square * 0.3)) {
      return vec3(constants.pattern_value);
    }
    return vec3(10000.0 / 80.0);
  }
  return vec3(0.0);
}

// The stand-in pixel a swapchain pixel shows.
ivec2 StandInPixel(ivec2 pixel) {
  ivec2 size = ivec2(constants.size);
  uint rotation = (constants.output_flags >> kRotationShift) & 3u;
  if (rotation == kRotation90) {
    return ivec2(pixel.y, size.y - 1 - pixel.x);
  }
  if (rotation == kRotation270) {
    return ivec2(size.x - 1 - pixel.y, pixel.x);
  }
  return pixel;
}

void main() {
  ivec2 pixel = StandInPixel(ivec2(gl_FragCoord.xy));
  vec2 frag = vec2(pixel) + 0.5;
  vec4 sdr = texelFetch(stand_in, pixel, 0);
  float ui = clamp(sdr.a, 0.0, 1.0);
  vec3 linear = sdr.rgb;
  if (constants.compare_x >= 0.0 && frag.x < constants.compare_x) {
    // The SDR reference half.
    vec3 reference = linear * constants.paper_white_scale;
    if (frag.x > constants.compare_x - 3.0) {
      reference = vec3(constants.paper_white_scale);
    }
    output_color = Output(reference);
    return;
  }
  if (constants.gamma > 0.0) {
    linear = pow(SrgbEncode(linear), vec3(constants.gamma));
  }
  float luminance = dot(linear, vec3(0.2126, 0.7152, 0.0722));
  float brightness = mix(luminance, max3(linear), constants.max_channel_weight);
  float t = clamp((brightness - constants.knee) * constants.knee_range_inv, 0.0, 1.0);
  // Smooth from the knee, reaching the peak at SDR white. Scaling all three
  // channels together keeps the hue.
  float boost = 1.0 + constants.highlight_scale * pow(t, constants.curve) * (1.0 - ui);
  vec3 color = linear * (constants.paper_white_scale * boost);
  if (constants.pattern != kPatternNone) {
    // The pattern replaces the image; the menu stays on top of it.
    color = mix(Pattern(frag), linear * constants.paper_white_scale, ui);
  }
  output_color = Output(color);
}
