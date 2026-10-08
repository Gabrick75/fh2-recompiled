// rex: resolve shortcut conversion (VulkanTextureCache::Atalho). Reads a region of a color render target (at
// the render scale) and writes it, multiplied by the resolve's exponent bias, packed in the host format of the
// destination texture, so it can be copied into an image of that format and blitted into the texture.
//
// Compiled with tools/compilar_shaders.sh into vulkan_spirv/atalho_conversao_cs.h.

#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(std430, set = 0, binding = 0) writeonly buffer AtalhoDest {
  uint words[];
} dest;

layout(set = 1, binding = 0) uniform texture2D source;

layout(push_constant) uniform AtalhoConstants {
  ivec2 source_origin;  // host pixels in the render target
  uvec2 size;           // host pixels
  float exp_factor;     // 2^exp_bias
  uint mode;            // 0: R8G8B8A8_UNORM, 1: A2B10G10R10_UNORM_PACK32, 2: R16G16B16A16_SFLOAT
  uint pitch;           // texels per row in the destination
  uint swap;            // copy_dest_swap: red and blue exchanged in the destination
} constants;

void main() {
  uvec2 xy = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(xy, constants.size))) {
    return;
  }
  vec4 color = texelFetch(source, constants.source_origin + ivec2(xy), 0) * constants.exp_factor;
  if (constants.swap != 0u) {
    color = color.bgra;
  }
  uint index = xy.y * constants.pitch + xy.x;
  if (constants.mode == 0u) {
    dest.words[index] = packUnorm4x8(color);
  } else if (constants.mode == 1u) {
    vec4 c = clamp(color, 0.0, 1.0);
    dest.words[index] = uint(c.r * 1023.0 + 0.5) | (uint(c.g * 1023.0 + 0.5) << 10u) |
                        (uint(c.b * 1023.0 + 0.5) << 20u) | (uint(c.a * 3.0 + 0.5) << 30u);
  } else {
    dest.words[index * 2u] = packHalf2x16(color.rg);
    dest.words[index * 2u + 1u] = packHalf2x16(color.ba);
  }
}
