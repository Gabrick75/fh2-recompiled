#version 460

// present_frame_gen: the motion search, one vector per texel of a pyramid level. Compiled with
// tools/compilar_shaders.sh.
//
// Symmetric block matching, the kind TVs use for frame rate conversion: the vector d belongs to the frame
// in the middle, whose block came from p - d in the previous frame and goes to p + d in the current one.
// So every pixel of the frame in between has a vector and there are no holes to fill. The block is 4x4
// texels of the level (64x64 guest pixels at 1/16, 16x16 at 1/4): from one texel before this one to two
// after, the nearest a block of even size gets to being centered.
//
// The coarsest level searches every whole texel within the range. The finer ones take the vectors of the
// level above at this texel and its four neighbors, plus zero (the HUD and the sky), keep the best and
// look around it one step in each of the eight directions. Vectors are stored in guest pixels.

layout(push_constant) uniform FrameGenSearchConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) vec2 level_texel;  // 1 / this level's size
  vec2 parent_texel;                     // 1 / the size of the level above (refinement)
  float guest_pixels_per_texel;          // 4, 8 or 16
  float refine_step;                     // in texels of this level; 0 = full search
  int search_range;                      // full search: +- texels
  float padding;
  vec4 motion_params;                    // motionBlurParams1's y, z, w, and 1 if there is velocity (GameVector)
  vec2 motion_region;
  vec2 motion_scale;
} constants;

layout(set = 1, binding = 0) uniform texture2D previous_luma;
layout(set = 1, binding = 1) uniform texture2D current_luma;
layout(set = 1, binding = 2) uniform texture2D parent_vectors;
layout(set = 1, binding = 3) uniform sampler linear_sampler;
layout(set = 1, binding = 4) uniform sampler nearest_sampler;

layout(location = 0) out vec2 vector_out;

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

// The sum of absolute differences of the block, in texels of this level, plus a small price per texel of
// length, so a flat area (the sky, the road) keeps the shortest vector. A whole-texel vector keeps the block
// on texel corners, so each 2x2 of it is one gather (8 reads instead of 32); the half-texel steps of the
// last refinement filter instead.
float Cost(vec2 uv, vec2 d) {
  vec2 o = d * constants.level_texel;
  float sad = 0.0;
  if (all(equal(d, floor(d)))) {
    for (int y = 0; y < 2; ++y) {
      for (int x = 0; x < 2; ++x) {
        // The corners in the middle of texels -1 and 0, and of 1 and 2.
        vec2 corner = uv + (vec2(x, y) * 2.0 - 0.5) * constants.level_texel;
        vec4 difference = abs(textureGather(sampler2D(previous_luma, nearest_sampler), corner - o, 0) -
                              textureGather(sampler2D(current_luma, nearest_sampler), corner + o, 0));
        sad += difference.x + difference.y + difference.z + difference.w;
      }
    }
  } else {
    for (int y = 0; y < 4; ++y) {
      for (int x = 0; x < 4; ++x) {
        vec2 t = (vec2(x, y) - 1.0) * constants.level_texel;
        sad += abs(textureLod(sampler2D(previous_luma, linear_sampler), uv + t - o, 0.0).r -
                   textureLod(sampler2D(current_luma, linear_sampler), uv + t + o, 0.0).r);
      }
    }
  }
  // A block that would come from outside the frame matches the clamped edge by chance.
  vec2 a = uv - o, b = uv + o;
  if (any(lessThan(min(a, b), vec2(0.0))) || any(greaterThan(max(a, b), vec2(1.0)))) {
    sad += 2.0;
  }
  // The full search at the coarsest level pays more per texel: there the 4x4 block is 128 guest pixels of a
  // small image, and a far block matching by chance (the sky, a crowd) is what made whole regions jump.
  return sad + (constants.refine_step == 0.0 ? 0.2 : 0.01) * length(d);
}

vec2 Parent(vec2 uv) {
  return textureLod(sampler2D(parent_vectors, nearest_sampler), uv, 0.0).rg /
         constants.guest_pixels_per_texel;
}

void main() {
  vec2 uv = gl_FragCoord.xy * constants.level_texel;
  vec2 best = vec2(0.0);
  float best_cost = Cost(uv, best);
  if (constants.refine_step == 0.0) {
    int r = constants.search_range;
    for (int y = -r; y <= r; ++y) {
      for (int x = -r; x <= r; ++x) {
        vec2 d = vec2(x, y);
        float cost = Cost(uv, d);
        if (cost < best_cost) {
          best_cost = cost;
          best = d;
        }
      }
    }
    // The game's motion, as the shader computed it for this block's center.
    vec2 game = GameVector(uv) / constants.guest_pixels_per_texel;
    float cost = Cost(uv, game) - 0.2 * length(game) + 0.01 * length(game);
    if (constants.motion_params.w != 0.0 && cost < best_cost) {
      best_cost = cost;
      best = game;
    }
  } else {
    vec2 p = constants.parent_texel;
    vec2 own = Parent(uv);
    vec2 candidates[6] = vec2[6](own, Parent(uv + vec2(-p.x, 0.0)), Parent(uv + vec2(p.x, 0.0)),
                                 Parent(uv + vec2(0.0, -p.y)), Parent(uv + vec2(0.0, p.y)),
                                 GameVector(uv) / constants.guest_pixels_per_texel);
    for (int i = 0; i < (constants.motion_params.w != 0.0 ? 6 : 5); ++i) {
      // A little in favor of this texel's own vector, so a smooth field stays smooth; the game's motion as
      // much (it is the real one where the game has it).
      float cost = Cost(uv, candidates[i]) + (i == 0 || i == 5 ? 0.0 : 0.02);
      if (cost < best_cost) {
        best_cost = cost;
        best = candidates[i];
      }
    }
    vec2 center = best;
    float s = constants.refine_step;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        if (x == 0 && y == 0) {
          continue;
        }
        vec2 d = center + vec2(x, y) * s;
        float cost = Cost(uv, d);
        if (cost < best_cost) {
          best_cost = cost;
          best = d;
        }
      }
    }
  }
  vector_out = best * constants.guest_pixels_per_texel;
}
