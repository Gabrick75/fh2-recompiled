#version 460
#extension GL_EXT_samplerless_texture_functions : require

// present_frame_gen: the motion of every pixel the way FH2's velocity pass (p_003929) computes it, from its own depth
// texture and matrix table, but kept as it is: without the motion blur's curve and clamp, and whether the blur is on
// or not (the game only keeps its velocity target usable while the blur is on). Compiled with
// tools/compilar_shaders.sh.
//
// The steps are the pass's, in its order: its depth texture is the scene's depth and stencil (the native renderer packs
// them into the game's 8_8_8_8 view; here they are read from the D24S8 image as they are), the stencil being the
// object id (0 or 128 and up: the world; 1-127: a car); the pixel is moved
// back to its view position with QAndNegNearQAndScale and projected into the previous frame with the object's rows
// of invCurrentVPThenPrevVP. The result is the previous minus the current position, in the units of the pass's
// screen position (x right and y up from the center), turned into the half vector of the other frame_gen shaders
// with the same scale as the decoded velocity.

layout(push_constant) uniform FrameGenMotionConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) vec2 dest_texel;  // 1 / this target's size (half the guest output)
  vec2 depth_region;                    // the part of the depth texture the screen covers, in texture coordinates
  vec2 scale;                           // guest pixels per unit of motion (present_frame_gen_escala_movimiento_x/y)
} constants;

layout(set = 2, binding = 0) uniform texture2D depth_texture;
layout(set = 2, binding = 1) uniform sampler nearest_sampler;
layout(set = 2, binding = 3) uniform utexture2D stencil_texture;
// c32-c255 (invCurrentVPThenPrevVP), c124 (QAndNegNearQAndScale), then the exponent adjust in row 225's x.
layout(set = 2, binding = 2, std140) uniform GameConstants {
  vec4 rows[226];
} game;

layout(location = 0) out vec2 vector_out;

vec4 Row(int index) { return index >= 0 && index < 224 ? game.rows[index] : vec4(0.0); }

void main() {
  vec2 uv = gl_FragCoord.xy * constants.dest_texel;
  // The pass's screen position: x right, y up, -1 to 1.
  vec2 position = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  vec4 q = game.rows[224];  // QAndNegNearQAndScale
  ivec2 texel = ivec2(uv * constants.depth_region * vec2(textureSize(depth_texture, 0)));
  // What the pass decodes from the 8_8_8_8 view: the depth (24-bit float there) and the stencil / 255.
  float depth = texelFetch(depth_texture, texel, 0).r;
  float stencil = float(texelFetch(stencil_texture, texel, 0).r & 0xFFu) / 255.0;
  if (stencil >= 0.5) {  // the threshold as nfsmw_nativo_shaders.cpp corrects it
    stencil = 0.0;
  }
  float object = floor(stencil * 85.0 + 0.5);

  float inverse = 1.0 / (1.0 - q.x - depth);
  vec3 view = vec3(position.y * q.y * inverse, q.y * inverse * (1.0 - depth), q.y * inverse);
  int a0 = int(clamp(floor(4.0 * object + 0.5), -256.0, 255.0));
  vec4 m0 = Row(a0), m1 = Row(a0 + 1), m3 = Row(a0 + 3);
  // view = (r3.x, r3.y, r3.z); r2.xyz = r3.z * (m0.x, m3.x, m1.x).
  float x = view.z * m0.x * position.x + dot(view, m0.yzw);
  float y = view.z * m1.x * position.x + dot(view.zyx, m1.wzy);
  float w = view.z * m3.x * position.x + dot(view, m3.yzw);
  vec2 previous_minus_current = vec2(x, y) / w - position;
  // A pixel the pass would not move (no depth: the sky) stays still.
  if (!(abs(previous_minus_current.x) < 4.0 && abs(previous_minus_current.y) < 4.0)) {
    previous_minus_current = vec2(0.0);
  }
  vector_out = -0.5 * previous_minus_current * constants.scale;
}
