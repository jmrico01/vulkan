#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec3 inNormal;

layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D texSampler;

void main() {
    vec3 colorAmbient = vec3(0.05f, 0.05f, 0.05f);
    vec3 colorLight = vec3(0.9f, 0.9f, 0.9f);
    vec3 lightDir = normalize(vec3(0.2f, 0.3f, 1.0f));

    float lightBounce = max(dot(-lightDir, inNormal), 0.0);

    outColor = vec4(colorAmbient + colorLight * lightBounce, 1.0);
}
