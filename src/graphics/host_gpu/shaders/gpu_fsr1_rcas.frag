#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform sampler2D fsr_input;
layout(push_constant) uniform FsrRcasConstants {
    uvec4 con;
    ivec2 output_offset;
    uvec2 padding;
} parameters;

layout(location = 0) out vec4 output_color;

#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_RCAS_F 1
#define FSR_RCAS_PASSTHROUGH_ALPHA 1

AF4 FsrRcasLoadF(ASU2 p) {
    ivec2 size = textureSize(fsr_input, 0);
    ivec2 position = clamp(ivec2(p), ivec2(0), size - ivec2(1));
    return texelFetch(fsr_input, position, 0);
}

void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#include "ffx_fsr1.h"

void main() {
    AF1 red, green, blue, alpha;
    uvec2 output_position = uvec2(ivec2(gl_FragCoord.xy) - parameters.output_offset);
    FsrRcasF(red, green, blue, alpha, output_position, parameters.con);
    output_color = vec4(red, green, blue, alpha);
}
