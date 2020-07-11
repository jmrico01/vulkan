#version 450

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;

layout(location = 0) out vec2 outUv;

layout(binding = 0) uniform UniformBufferObject {
    vec2 origin;
    vec2 size;
} ubo;

void main() {
    outUv = inUv;

    gl_Position = vec4(inPosition * ubo.size + ubo.origin, 0.0, 1.0);
}