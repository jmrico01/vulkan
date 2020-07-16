#version 450

// Vertex attributes
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

// Instance attributes
layout(location = 3) in mat4 model;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
} ubo;

void main() {
    outNormal = inNormal;
    outColor = inColor;

	vec4 pos = ubo.proj * ubo.view * /*model * */ vec4(inPosition, 1.0);
    gl_Position = pos;
}
