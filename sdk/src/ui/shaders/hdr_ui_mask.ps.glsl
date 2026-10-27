#version 460

// From arcanite24/shiftglue-sdk (BSD-3-Clause), the HDR output of Pinyon
// Shift. Compiled with tools/compilar_shaders.sh.

// present_hdr: replaces the alpha of the painted guest output with the guest
// output image's own alpha, how much of each pixel is the game's HUD, before
// the host UI adds its own coverage.

layout(push_constant) uniform HdrUIMaskConstants {
  // After the rectangle constants of the vertex shader, like the bilinear
  // effect's constants.
  layout(offset = 16) ivec2 output_offset;
  vec2 output_size_inv;
} constants;

layout(set = 0, binding = 0) uniform texture2D guest_output;
layout(set = 0, binding = 1) uniform sampler guest_output_sampler;

layout(location = 0) out vec4 output_color;

void main() {
  vec2 uv = (gl_FragCoord.xy - vec2(constants.output_offset)) * constants.output_size_inv;
  output_color = vec4(0.0, 0.0, 0.0,
                      texture(sampler2D(guest_output, guest_output_sampler), uv).a);
}
