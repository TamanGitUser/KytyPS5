#version 450
#extension GL_GOOGLE_include_directive : require
layout(set = 0, binding = 0) uniform sampler2D fsr_input;
layout(push_constant) uniform FsrEasuConstants {
    uvec4 con0; uvec4 con1; uvec4 con2; uvec4 con3; ivec2 output_offset;
    uvec2 padding;
} parameters;
layout(location = 0) out vec4 output_color;
#define A_GPU 1
#define A_GLSL 1
#include "ffx_a.h"
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return AF4(textureGather(fsr_input, p, 0)); }
AF4 FsrEasuGF(AF2 p) { return AF4(textureGather(fsr_input, p, 1)); }
AF4 FsrEasuBF(AF2 p) { return AF4(textureGather(fsr_input, p, 2)); }
#include "ffx_fsr1.h"
void main() {
    uvec2 output_position = uvec2(ivec2(gl_FragCoord.xy) - parameters.output_offset);
    AF3 color;
    FsrEasuF(color, output_position, parameters.con0, parameters.con1, parameters.con2, parameters.con3);
    output_color = vec4(color, 1.0);
}