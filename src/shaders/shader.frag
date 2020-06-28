#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D texSampler;

void main() {
    // outColor = vec4(inColor * texture(texSampler, inUv).rgb, 1.0);
    outColor = vec4(1.0, 1.0, 1.0, 1.0);
}
