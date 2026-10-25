#version 460
#extension GL_EXT_samplerless_texture_functions : require

// present_hdr: FH2's HUD in the output's alpha (1 HUD, 0 scene), drawn after the output pass with only alpha
// written, so the presenter's HDR pass expands the scene's highlights and leaves the HUD at the paper white.
// From Pinyon Shift's hdr_hud_mask.cs.glsl (arcanite24/shiftglue-sdk, BSD-3-Clause): the frame against the
// render target as it was before the first HUD draw (DestinosVulkan::Fh2HdrFotoAntesDelHud). Every pixel
// the HUD changes counts, by a pixel more to cover its antialiased edges. Both are copies of the same 8-bit
// render target, so the scene's pixels are equal to the bit. Compiled with tools/compilar_shaders.sh.

layout(push_constant) uniform Constantes {
  // After the rectangle constants of the vertex shader.
  layout(offset = 16) vec2 escala;  // source pixels per output pixel
  ivec2 limite;                     // last texel both sources have
} constantes;

layout(set = 0, binding = 0) uniform texture2D cuadro;  // the frame, with the HUD
layout(set = 0, binding = 1) uniform texture2D escena;  // before the HUD

layout(location = 0) out vec4 color;

bool Cambio(ivec2 pixel) {
  pixel = clamp(pixel, ivec2(0), constantes.limite);
  vec3 diferencia = abs(texelFetch(cuadro, pixel, 0).rgb - texelFetch(escena, pixel, 0).rgb);
  // Half an 8-bit step.
  return max(diferencia.r, max(diferencia.g, diferencia.b)) > 0.5 / 255.0;
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy * constantes.escala);
  bool hud = false;
  for (int y = -1; y <= 1 && !hud; ++y) {
    for (int x = -1; x <= 1 && !hud; ++x) {
      hud = Cambio(pixel + ivec2(x, y));
    }
  }
  color = vec4(0.0, 0.0, 0.0, hud ? 1.0 : 0.0);
}
