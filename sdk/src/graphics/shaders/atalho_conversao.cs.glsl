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

// The render target's transfer view: float, or unsigned integer for the formats whose bits must stay exact
// (16-bit and 32-bit floats). Only the declaration matching the view is read (source_kind).
layout(set = 1, binding = 0) uniform texture2D source;
layout(set = 1, binding = 0) uniform utexture2D source_uint;
layout(set = 1, binding = 0) uniform texture2DMS source_ms;

layout(push_constant) uniform AtalhoConstants {
  ivec2 source_origin;  // host pixels in the render target
  uvec2 size;           // host pixels
  float exp_factor;     // 2^exp_bias
  uint mode;            // 0: R8G8B8A8_UNORM, 1: A2B10G10R10_UNORM_PACK32, 2: R16G16B16A16_SFLOAT, 3: R32_SFLOAT
  uint pitch;           // texels per row in the destination
  uint swap;            // copy_dest_swap: red and blue exchanged in the destination
  uint source_kind;     // 0: float view (color, or depth in .r), 1: R16G16B16A16_UINT view of halves,
                        // 2: R32_UINT view of a float, 3: multisampled float view
  int sample_index;     // host sample read with source_kind 3
} constants;

void main() {
  uvec2 xy = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(xy, constants.size))) {
    return;
  }
  ivec2 position = constants.source_origin + ivec2(xy);
  vec4 color;
  if (constants.source_kind == 0u) {
    color = texelFetch(source, position, 0);
  } else if (constants.source_kind == 1u) {
    uvec4 halves = texelFetch(source_uint, position, 0);
    color = vec4(unpackHalf2x16(halves.r | (halves.g << 16u)), unpackHalf2x16(halves.b | (halves.a << 16u)));
  } else if (constants.source_kind == 2u) {
    color = vec4(uintBitsToFloat(texelFetch(source_uint, position, 0).r), 0.0, 0.0, 1.0);
  } else {
    color = texelFetch(source_ms, position, constants.sample_index);
  }
  color *= constants.exp_factor;
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
  } else if (constants.mode == 3u) {
    dest.words[index] = floatBitsToUint(color.r);
  } else {
    dest.words[index * 2u] = packHalf2x16(color.rg);
    dest.words[index * 2u + 1u] = packHalf2x16(color.ba);
  }
}
