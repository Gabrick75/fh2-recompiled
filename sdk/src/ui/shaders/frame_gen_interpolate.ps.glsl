#version 460

// present_frame_gen: the frame in the middle, at the guest output's size. Compiled with
// tools/compilar_shaders.sh.
//
// Each pixel picks its vector among six: the field interpolated here, the four nearest vectors of the
// field as they are (an object's edge must not get the average of the object and the background) and zero
// (the HUD, which does not move, matches it exactly). The one whose two ends look most alike wins. The
// color is the average of both ends; where even the best vector does not match (something appears or
// disappears between the frames), the current frame's end, which at worst repeats the current frame in
// that spot instead of a ghost of both.

layout(push_constant) uniform FrameGenInterpolateConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) vec2 guest_texel;  // 1 / the guest output size
  vec2 vector_size;                      // the vector field's size (a quarter of the guest output)
} constants;

layout(set = 0, binding = 0) uniform texture2D current_frame;
layout(set = 0, binding = 1) uniform sampler current_sampler;
layout(set = 1, binding = 0) uniform texture2D previous_frame;
layout(set = 1, binding = 2) uniform texture2D vectors;
layout(set = 1, binding = 3) uniform sampler linear_sampler;
layout(set = 1, binding = 4) uniform sampler nearest_sampler;

layout(location = 0) out vec4 color_out;

vec4 Previous(vec2 uv) { return textureLod(sampler2D(previous_frame, linear_sampler), uv, 0.0); }
vec4 Current(vec2 uv) { return textureLod(sampler2D(current_frame, current_sampler), uv, 0.0); }

bool Inside(vec2 uv) { return all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))); }

// How unlike the two ends of the vector are, on three pixels along a diagonal.
float Mismatch(vec2 uv, vec2 d) {
  vec2 o = d * constants.guest_texel;
  if (!Inside(uv - o) || !Inside(uv + o)) {
    return 1.0;
  }
  vec2 t = 1.5 * constants.guest_texel;
  float e = 0.0;
  e += dot(abs(Previous(uv - o).rgb - Current(uv + o).rgb), vec3(1.0));
  e += dot(abs(Previous(uv + vec2(t.x, -t.y) - o).rgb - Current(uv + vec2(t.x, -t.y) + o).rgb), vec3(1.0));
  e += dot(abs(Previous(uv + vec2(-t.x, t.y) - o).rgb - Current(uv + vec2(-t.x, t.y) + o).rgb), vec3(1.0));
  return e * (1.0 / 9.0);
}

void main() {
  vec2 uv = gl_FragCoord.xy * constants.guest_texel;
  vec2 vector_texel = 1.0 / constants.vector_size;
  vec2 base = floor(uv * constants.vector_size - 0.5) + 0.5;
  vec2 candidates[6] = vec2[6](
      textureLod(sampler2D(vectors, linear_sampler), uv, 0.0).rg,
      textureLod(sampler2D(vectors, nearest_sampler), base * vector_texel, 0.0).rg,
      textureLod(sampler2D(vectors, nearest_sampler), (base + vec2(1.0, 0.0)) * vector_texel, 0.0).rg,
      textureLod(sampler2D(vectors, nearest_sampler), (base + vec2(0.0, 1.0)) * vector_texel, 0.0).rg,
      textureLod(sampler2D(vectors, nearest_sampler), (base + vec2(1.0, 1.0)) * vector_texel, 0.0).rg,
      vec2(0.0));
  vec2 best = vec2(0.0);
  float best_mismatch = 1e9;
  for (int i = 0; i < 6; ++i) {
    // The smooth field is a little preferred: it does not shimmer from one pixel to the next.
    float m = Mismatch(uv, candidates[i]) * (i == 0 ? 0.9 : 1.0);
    if (m < best_mismatch) {
      best_mismatch = m;
      best = candidates[i];
    }
  }
  vec2 o = best * constants.guest_texel;
  bool previous_inside = Inside(uv - o), current_inside = Inside(uv + o);
  vec4 a = Previous(previous_inside ? uv - o : uv);
  vec4 b = Current(current_inside ? uv + o : uv);
  vec4 middle = previous_inside && current_inside ? (a + b) * 0.5 : (current_inside ? b : a);
  vec4 color = mix(middle, b, smoothstep(0.06, 0.2, best_mismatch));
  // The HUD coverage (present_hdr's mask) of either frame.
  color.a = max(a.a, b.a);
  color_out = color;
}
