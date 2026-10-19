// FH2's resolves with an exponent bias (RB_COPY_DEST_INFO bits 16-21): the render target's region times 2^bias,
// written to the resolved texture, as the Xbox 360 does when it copies out of the EDRAM. One variant per destination
// format (FORMATO); compiled by tools/xenos/compilar_copia_escala.py into fh2_copia_escala_spv.h.
#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform texture2D origen;
layout(set = 0, binding = 1, FORMATO) uniform writeonly image2D destino;

layout(push_constant) uniform Constantes {
  ivec2 origen_xy;
  ivec2 destino_xy;
  ivec2 tamano;
  float escala;
  // The render target's largest value (31.875 for 2_10_10_10_FLOAT, 7e3); applied before the scale. Negative: the
  // fixed point 16_16 / 16_16_16_16 range, -32 to 32 on all four channels.
  float maximo;
} c;

void main() {
  const ivec2 i = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(i, c.tamano))) {
    return;
  }
  vec4 texel = texelFetch(origen, c.origen_xy + i, 0);
  // A NaN is stored as 0: min(NaN, maximo) may return maximo, and a NaN in one channel then saturated to 1 in the
  // unorm texture (green in FH2's bloom pyramid, which feeds itself back frame after frame).
  texel = mix(texel, vec4(0.0), isnan(texel));
  if (c.maximo < 0.0) {
    texel = clamp(texel, vec4(c.maximo), vec4(-c.maximo));
  } else {
    texel.rgb = min(texel.rgb, vec3(c.maximo));
  }
  imageStore(destino, c.destino_xy + i, texel * c.escala);
}
