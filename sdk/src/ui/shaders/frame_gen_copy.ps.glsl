#version 460
#extension GL_EXT_samplerless_texture_functions : require

// present_frame_gen: keeps the guest output that was just presented, for the next frame in the middle
// (the mailbox reuses its images). Compiled with tools/compilar_shaders.sh.

layout(set = 0, binding = 0) uniform texture2D guest_output;

layout(location = 0) out vec4 color_out;

void main() { color_out = texelFetch(guest_output, ivec2(gl_FragCoord.xy), 0); }
