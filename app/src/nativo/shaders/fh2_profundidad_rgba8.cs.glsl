// FH2 resolves its scene depth and its motion blur reads it as an 8_8_8_8 texture: the raw bits of the EDRAM word
// (depth << 8 | stencil), with the 24-bit depth in 20e4 float for D24FS8. This builds that texture from the depth
// render target: R = stencil, G/B/A = depth bits 0-7/8-15/16-23, as the console's fetch sees the word.
// Compiled by tools/xenos/compilar_copia_escala.py into fh2_copia_escala_spv.h.
#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform texture2D profundidad;
layout(set = 0, binding = 1) uniform utexture2D stencil;
layout(set = 0, binding = 2, rgba8) uniform writeonly image2D destino;

layout(push_constant) uniform Constantes {
  ivec2 origen_xy;
  ivec2 destino_xy;
  ivec2 tamano;
  uint flotante;  // bit 0: D24FS8 (20e4 depth); bit 1: stencil read as 0 (debug)
} c;

// As Xenia's Float32To20e4 with round to nearest even.
uint A20e4(float f) {
  if (!(f > 0.0)) {
    return 0u;
  }
  uint u = floatBitsToUint(f);
  if (u >= 0x3FFFFFF8u) {
    return 0xFFFFFFu;
  }
  if (u < 0x38800000u) {
    u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23), 24u);
  } else {
    u += 0xC8000000u;
  }
  u += 3u + ((u >> 3) & 1u);
  return (u >> 3) & 0xFFFFFFu;
}

void main() {
  const ivec2 i = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(i, c.tamano))) {
    return;
  }
  const ivec2 p = c.origen_xy + i;
  const float z = texelFetch(profundidad, p, 0).r;
  uint d = (c.flotante & 1u) != 0u ? A20e4(z) : uint(clamp(z, 0.0, 1.0) * 16777215.0 + 0.5);
  const uint s = (c.flotante & 2u) != 0u ? 0u : texelFetch(stencil, p, 0).r & 0xFFu;
  if ((c.flotante & 8u) != 0u) {
    d = 0x800000u;  // debug: a constant depth
  }
  vec4 bytes = vec4(float(s), float(d & 0xFFu), float((d >> 8) & 0xFFu), float(d >> 16));
  if ((c.flotante & 4u) != 0u) {
    bytes = bytes.wzyx;  // debug: the word's bytes in the other order
  }
  imageStore(destino, c.destino_xy + i, bytes * (1.0 / 255.0));
}
