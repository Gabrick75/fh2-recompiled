#version 460
#extension GL_EXT_samplerless_texture_functions : require

// present_frame_gen: the motion of every pixel the way FH2's velocity pass (p_003929) computes it, from its own depth
// and matrix table, but kept as it is: without the motion blur's curve and clamp, and whether the blur is on or not
// (the game only keeps its velocity target usable while the blur is on). Compiled with tools/compilar_shaders.sh.
//
// The steps are the pass's, in its order: its depth texture is the scene's depth and stencil (the native renderer
// packs them into the game's 8_8_8_8 view; here they are read from the D24S8 image as they are), the stencil being
// the object id (0 or 128 and up: the world; 1-127: a car); the pixel is moved back to its view position with
// QAndNegNearQAndScale and projected into the previous frame with the object's rows of invCurrentVPThenPrevVP. The
// result is the previous minus the current position, in the units of the pass's screen position (x right and y up
// from the center), turned into the half vector of the other frame_gen shaders with the same scale as the decoded
// velocity.
//
// Dilated by depth, as FSR 3 does with the game's vectors: each pixel takes the motion of the nearest surface in its
// 3x3. A pole or a branch a pixel or two wide then lends its motion to the pixels around it, which is what lets the
// frame in the middle find it where it will be (the background around it would carry it nowhere).

layout(push_constant) uniform FrameGenMotionConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) vec2 dest_texel;  // 1 / this target's size (the guest output's)
  vec2 depth_region;                    // the part of the depth texture the screen covers, in texture coordinates
  vec2 scale;                           // guest pixels per unit of motion (present_frame_gen_escala_movimiento_x/y)
  float padding;
  float debug_view;                     // 1: depth * 1000 and stencil in place of the motion, for the dumps
} constants;

layout(set = 2, binding = 0) uniform texture2D depth_texture;
layout(set = 2, binding = 1) uniform sampler nearest_sampler;
layout(set = 2, binding = 3) uniform texture2D stencil_texture;  // the packed depth: R is the stencil / 255
// c32-c255 (invCurrentVPThenPrevVP), c124 (QAndNegNearQAndScale), then the exponent adjust in row 225's x.
layout(set = 2, binding = 2, std140) uniform GameConstants {
  vec4 rows[226];
} game;

layout(location = 0) out vec2 vector_out;

vec4 Row(int index) { return index >= 0 && index < 224 ? game.rows[index] : vec4(0.0); }

// The half vector of the depth texel `texel` (the pass's math), and its depth.
vec2 Motion(ivec2 texel, ivec2 size, out float depth) {
  texel = clamp(texel, ivec2(0), size - 1);
  vec2 uv = (vec2(texel) + 0.5) / (vec2(size) * constants.depth_region);
  // The pass's screen position: x right, y up, -1 to 1.
  vec2 position = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  vec4 q = game.rows[224];  // QAndNegNearQAndScale
  // The game renders y up, so the scene's depth is upside down in the image (as its velocity target is): the screen's
  // top row is the screen area's last one.
  ivec2 stored = ivec2(texel.x, int(vec2(size).y * constants.depth_region.y) - 1 - texel.y);
  depth = texelFetch(depth_texture, stored, 0).r;
  float stencil = texelFetch(stencil_texture, stored, 0).r;
  if (stencil >= 0.5) {  // the threshold as nfsmw_nativo_shaders.cpp corrects it
    stencil = 0.0;
  }
  float object = floor(stencil * 85.0 + 0.5);
  float inverse = 1.0 / (1.0 - q.x - depth);
  vec3 view = vec3(position.y * q.y * inverse, q.y * inverse * (1.0 - depth), q.y * inverse);
  int a0 = int(clamp(floor(4.0 * object + 0.5), -256.0, 255.0));
  vec4 m0 = Row(a0), m1 = Row(a0 + 1), m3 = Row(a0 + 3);
  float x = view.z * m0.x * position.x + dot(view, m0.yzw);
  float y = view.z * m1.x * position.x + dot(view.zyx, m1.wzy);
  float w = view.z * m3.x * position.x + dot(view, m3.yzw);
  vec2 previous_minus_current = vec2(x, y) / w - position;
  // A pixel the pass would not move sensibly (the far plane, a degenerate w) stays still.
  if (!(abs(previous_minus_current.x) < 4.0 && abs(previous_minus_current.y) < 4.0)) {
    previous_minus_current = vec2(0.0);
  }
  return -0.5 * previous_minus_current * constants.scale;
}

void main() {
  ivec2 size = textureSize(depth_texture, 0);
  vec2 uv = gl_FragCoord.xy * constants.dest_texel;
  ivec2 center = ivec2(uv * constants.depth_region * vec2(size));
  if (constants.debug_view != 0.0) {
    ivec2 stored = ivec2(center.x, int(vec2(size).y * constants.depth_region.y) - 1 - center.y);
    vec4 packed_texel = texelFetch(stencil_texture, stored, 0);
    // 2: the packed view's own depth bytes (G, then A) in place of the stencil, to compare its timing with the depth's.
    // 3: its B and A bytes, one of them the depth's high byte.
    if (constants.debug_view > 2.5) {
      vector_out = vec2(packed_texel.b, packed_texel.a) * 1000.0;
      return;
    }
    vector_out = vec2(texelFetch(depth_texture, stored, 0).r * 1000.0,
                      constants.debug_view > 1.5 ? packed_texel.g * 1000.0 : packed_texel.r * 255.0);
    return;
  }
  // FH2's Z is reversed: the pass's w is -near Q / (1 - Q - depth), so depth 1 is the near plane and 0 the far one.
  float nearest_depth = -1.0;
  vec2 best = vec2(0.0);
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      float depth;
      vec2 motion = Motion(center + ivec2(x, y), size, depth);
      if (depth > nearest_depth) {
        nearest_depth = depth;
        best = motion;
      }
    }
  }
  vector_out = best;
}
