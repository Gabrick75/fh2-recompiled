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
} c;

void main() {
  const ivec2 i = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(i, c.tamano))) {
    return;
  }
  imageStore(destino, c.destino_xy + i, texelFetch(origen, c.origen_xy + i, 0) * c.escala);
}
