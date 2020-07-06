#version 450
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec4 outColor;

layout(binding = 1) uniform sampler2D texSampler;

void main() {
    vec3 colorAmbient = vec3(0.05f, 0.05f, 0.05f);
    vec3 colorLight = vec3(0.9f, 0.9f, 0.9f);
    vec3 lightDir = normalize(vec3(0.2f, 0.3f, 1.0f));

    float lightBounce = max(dot(-lightDir, inNormal), 0.0);
    vec3 texColor = texture(texSampler, inUv).rgb;

	vec3 finalColor = (colorAmbient + colorLight * lightBounce) * texColor;

    // outColor = vec4(finalColor, 1.0);
    outColor = vec4(inColor + texture(texSampler, inUv).rgb, 1.0);
}
