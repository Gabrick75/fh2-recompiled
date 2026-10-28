#version 460

// present_frame_gen: one level of the luma pyramid the motion search runs on. Compiled with
// tools/compilar_shaders.sh.
//
// The first level (a quarter of the guest output) averages 4x4 guest pixels with four bilinear taps,
// each on the corner shared by 2x2 pixels; the next ones halve the previous level with one tap. Luma of
// the gamma-encoded color, which is what the eye compares.

layout(push_constant) uniform FrameGenLumaConstants {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) vec2 source_texel;  // 1 / the source size
  vec2 dest_texel;                        // 1 / this level's size
  int from_guest_output;                  // 1: the source is the guest output, 0: the level above
} constants;

layout(set = 0, binding = 0) uniform texture2D guest_output;
layout(set = 0, binding = 1) uniform sampler guest_output_sampler;
layout(set = 1, binding = 0) uniform texture2D source_luma;
layout(set = 1, binding = 3) uniform sampler linear_sampler;

layout(location = 0) out float luma;

void main() {
  vec2 uv = gl_FragCoord.xy * constants.dest_texel;
  if (constants.from_guest_output != 0) {
    vec2 t = constants.source_texel;
    vec3 color = texture(sampler2D(guest_output, guest_output_sampler), uv + vec2(-t.x, -t.y)).rgb +
                 texture(sampler2D(guest_output, guest_output_sampler), uv + vec2(t.x, -t.y)).rgb +
                 texture(sampler2D(guest_output, guest_output_sampler), uv + vec2(-t.x, t.y)).rgb +
                 texture(sampler2D(guest_output, guest_output_sampler), uv + vec2(t.x, t.y)).rgb;
    luma = dot(color * 0.25, vec3(0.2126, 0.7152, 0.0722));
  } else {
    luma = texture(sampler2D(source_luma, linear_sampler), uv).r;
  }
}
