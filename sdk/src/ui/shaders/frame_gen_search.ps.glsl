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
} constants;

layout(set = 1, binding = 0) uniform texture2D previous_luma;
layout(set = 1, binding = 1) uniform texture2D current_luma;
layout(set = 1, binding = 2) uniform texture2D parent_vectors;
layout(set = 1, binding = 3) uniform sampler linear_sampler;
layout(set = 1, binding = 4) uniform sampler nearest_sampler;

layout(location = 0) out vec2 vector_out;

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
  return sad + 0.01 * length(d);
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
  } else {
    vec2 p = constants.parent_texel;
    vec2 own = Parent(uv);
    vec2 candidates[5] = vec2[5](own, Parent(uv + vec2(-p.x, 0.0)), Parent(uv + vec2(p.x, 0.0)),
                                 Parent(uv + vec2(0.0, -p.y)), Parent(uv + vec2(0.0, p.y)));
    for (int i = 0; i < 5; ++i) {
      // A little in favor of this texel's own vector, so a smooth field stays smooth.
      float cost = Cost(uv, candidates[i]) + (i == 0 ? 0.0 : 0.02);
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
