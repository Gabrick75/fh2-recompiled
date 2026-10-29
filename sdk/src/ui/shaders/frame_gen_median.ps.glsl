#version 460

// present_frame_gen: the vector median of the 3x3 around each vector of the finest search level. Compiled
// with tools/compilar_shaders.sh.
//
// A block that matched by chance (a repeated texture, a flat patch) gets a vector unlike its neighbors, and
// from one frame to the next that is what made objects tremble. The vector median keeps the one of the nine
// closest to all the others: an outlier never wins, and the edge between two motions stays where it is
// (an average would smear it).

layout(push_constant) uniform FrameGenMedianConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) vec2 vector_texel;  // 1 / the vector field's size
} constants;

layout(set = 1, binding = 0) uniform texture2D vectors;
layout(set = 1, binding = 4) uniform sampler nearest_sampler;

layout(location = 0) out vec2 vector_out;

void main() {
  vec2 uv = gl_FragCoord.xy * constants.vector_texel;
  vec2 v[9];
  for (int y = 0; y < 3; ++y) {
    for (int x = 0; x < 3; ++x) {
      v[y * 3 + x] = textureLod(sampler2D(vectors, nearest_sampler),
                                uv + vec2(x - 1, y - 1) * constants.vector_texel, 0.0).rg;
    }
  }
  vec2 best = v[4];
  float best_sum = 1e30;
  for (int i = 0; i < 9; ++i) {
    float sum = 0.0;
    for (int j = 0; j < 9; ++j) {
      sum += length(v[i] - v[j]);
    }
    if (sum < best_sum) {
      best_sum = sum;
      best = v[i];
    }
  }
  vector_out = best;
}
