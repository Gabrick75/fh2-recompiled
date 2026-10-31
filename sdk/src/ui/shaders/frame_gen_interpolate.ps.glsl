#version 460

// present_frame_gen: the frame in the middle, at the guest output's size. Compiled with
// tools/compilar_shaders.sh.
//
// Each pixel picks its vector among six: the field interpolated here, the four nearest vectors of the
// field as they are (an object's edge must not get the average of the object and the background) and zero
// (the HUD, which does not move, matches it exactly). The smooth field wins unless another candidate's two
// ends look clearly more alike: picking per pixel on a near tie is what made surfaces shimmer. The color is
// the average of both ends. Where that is in doubt (the best vector does not match, or its two ends differ at
// the pixel itself), the previous frame as it is: the frame already on the screen, so that spot just does not
// change for one refresh. It moves at the game's rate there, but it cannot flicker, tear or bend.

layout(push_constant) uniform FrameGenInterpolateConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) vec2 guest_texel;  // 1 / the guest output size
  vec2 vector_size;                      // the vector field's size (a quarter of the guest output)
  vec4 motion_params;                    // motionBlurParams1's y, z, w, and 1 if there is velocity (GameVector)
  vec2 motion_region;
  vec2 motion_scale;
} constants;

layout(set = 0, binding = 0) uniform texture2D current_frame;
layout(set = 0, binding = 1) uniform sampler current_sampler;
layout(set = 1, binding = 0) uniform texture2D previous_frame;
layout(set = 1, binding = 2) uniform texture2D vectors;
layout(set = 1, binding = 3) uniform sampler linear_sampler;
layout(set = 1, binding = 4) uniform sampler nearest_sampler;

layout(location = 0) out vec4 color_out;

// The game's velocity (FH2's p_003929, nfsmw_nativo_destinos.cpp): drawn at half the guest output's size in the
// corner of a larger target, upside down, as the motion blur stores it: xy the direction (the blur vector, clamped,
// times 5 around 0.498), z the unclamped length times the blur gain g(L) = 1 + w / (1 + (L z)^y) of
// motionBlurParams1. Undone here into the half vector d of this file: the pixel's content moves by 2d from the
// previous frame to the current one. Zero where there is no velocity this frame (params.w = 0).
layout(set = 2, binding = 0) uniform texture2D game_velocity;
layout(set = 2, binding = 1) uniform sampler game_velocity_sampler;

vec2 GameVector(vec2 uv) {
  if (constants.motion_params.w == 0.0) {
    return vec2(0.0);
  }
  // Mode 2 (frame_gen_motion.ps.glsl): the half vector already computed from the velocity pass's depth and
  // matrices, top-down, at half the guest output's size.
  if (constants.motion_params.x < 0.0) {
    return textureLod(sampler2D(game_velocity, game_velocity_sampler), uv, 0.0).rg;
  }
  vec2 tc = vec2(uv.x, 1.0 - uv.y) * constants.motion_region;
  vec4 v = textureLod(sampler2D(game_velocity, game_velocity_sampler), tc, 0.0);
  vec2 s = (v.xy - 0.49803922) / 5.0;
  float py = constants.motion_params.x, pz = constants.motion_params.y, pw = constants.motion_params.z;
  float L = v.z / (1.0 + pw);
  for (int i = 0; i < 8; ++i) {
    L = v.z / (1.0 + pw / (1.0 + pow(max(L * pz, 0.0), py)));
  }
  float n = length(s);
  vec2 blur = n > 1e-5 ? s * (L / n) : vec2(0.0);
  // The shader's motion is previous minus current position, with x negated for the blur.
  vec2 previous_minus_current = vec2(-blur.x, blur.y);
  return -0.5 * previous_minus_current * constants.motion_scale;
}

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
  // The game's motion of this pixel: the velocity is stored at current-frame positions, so it is looked up again
  // where the pixel lands in the current frame.
  vec2 game = GameVector(uv);
  game = GameVector(uv + game * constants.guest_texel);
  vec2 best = vec2(0.0);
  float best_score = 1e9, best_mismatch = 1e9;
  for (int i = 0; i < (constants.motion_params.w != 0.0 ? 7 : 6); ++i) {
    vec2 candidate = i < 6 ? candidates[i] : game;
    float m = Mismatch(uv, candidate);
    // The smooth field is preferred: it does not shimmer from one pixel to the next. The game's motion more.
    float score = m * (i == 0 ? 0.7 : (i == 6 ? 0.6 : 1.0));
    if (score < best_score) {
      best_score = score;
      best_mismatch = m;
      best = candidate;
    }
  }
  /*
   * Mode 2 (frame_gen_motion.ps.glsl): the motion computed from the game's depth and matrices is exact for the scene,
   * so it decides alone. Letting the colors choose is what made thin things vanish: for a pole or a branch that moves,
   * the background's vector matches too (both of its ends land on the sky) and won. The motion is the current
   * frame's, so it is looked up where the pixel lands in the current frame, twice to settle.
   */
  bool authoritative = constants.motion_params.w != 0.0 && constants.motion_params.x < 0.0 &&
                       constants.motion_params.y > 0.5;  // present_frame_gen_movimiento_autoritario
  if (authoritative) {
    best = GameVector(uv);
    best = GameVector(uv + best * constants.guest_texel);
    best = GameVector(uv + best * constants.guest_texel);
    best_mismatch = 0.0;
  }
  vec2 o = best * constants.guest_texel;
  bool previous_inside = Inside(uv - o), current_inside = Inside(uv + o);
  vec4 a = Previous(previous_inside ? uv - o : uv);
  vec4 b = Current(current_inside ? uv + o : uv);
  vec4 middle = previous_inside && current_inside ? (a + b) * 0.5 : (current_inside ? b : a);
  /*
   * The doubt, and the conservative answer to it (the S23 tests: trees and signs kept flickering).
   *
   * The three-tap mismatch: a vector that does not carry the previous frame onto the current one. The two ends
   * at the pixel itself: a thin object (leaves, a pole, a sign's edge) whose vector is the background's passes
   * the three taps on the background around it, and the average of its ends is the object half see-through.
   * Either way, the previous frame unwarped, which is what the screen already shows.
   */
  vec3 ends = abs(a.rgb - b.rgb);
  float ends_max = max(ends.r, max(ends.g, ends.b));
  vec4 color;
  if (authoritative) {
    // With the right vector, ends that differ are something appearing or disappearing behind an object (or light
    // changing): the current frame's end alone, along the motion, instead of a see-through mix.
    color = mix(middle, b, smoothstep(0.1, 0.25, ends_max));
  } else {
    float doubt = max(smoothstep(0.03, 0.08, best_mismatch), smoothstep(0.08, 0.2, ends_max));
    color = mix(middle, Previous(uv), doubt);
  }
  /*
   * Glowing particles (sparks, fireflies, lights in the dark): small bright points that land somewhere else every
   * frame, so no vector follows them and the average of the two ends drops them. The screen recording of the S23
   * showed them gone from every frame in the middle, blinking at 30 Hz. A pixel much brighter than its ring in
   * either real frame keeps the brighter of the two, unwarped: the particles of both frames, never none.
   */
  const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);
  vec2 r = 2.0 * constants.guest_texel;
  vec4 p_here = Previous(uv);
  float ring_p = dot(Previous(uv + vec2(r.x, 0.0)).rgb + Previous(uv - vec2(r.x, 0.0)).rgb +
                     Previous(uv + vec2(0.0, r.y)).rgb + Previous(uv - vec2(0.0, r.y)).rgb, kLuma) * 0.25;
  float ring_c = dot(Current(uv + vec2(r.x, 0.0)).rgb + Current(uv - vec2(r.x, 0.0)).rgb +
                     Current(uv + vec2(0.0, r.y)).rgb + Current(uv - vec2(0.0, r.y)).rgb, kLuma) * 0.25;
  vec4 c_here = Current(uv);
  float spark = max(dot(p_here.rgb, kLuma) - ring_p, dot(c_here.rgb, kLuma) - ring_c);
  color.rgb = mix(color.rgb, max(color.rgb, max(p_here.rgb, c_here.rgb)), smoothstep(0.12, 0.25, spark));
  // The HUD: the guest output's alpha is its mask with FH2's native renderer (present_hdr's, which it also writes
  // for present_frame_gen; opaque elsewhere, so a frame without it is shown as it is). Where either frame has HUD,
  // the current frame unwarped: the HUD updates at the game's rate but stays whole, where the scene's motion
  // under it (or a minimap that turns) would bend it.
  vec4 current_here = Current(uv);
  float hud = max(current_here.a, Previous(uv).a);
  color = mix(color, current_here, step(0.5, hud));
  color.a = max(hud, max(a.a, b.a));
  color_out = color;
}
