#version 450

// Vertex attributes
layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;

// Instance attributes
layout(location = 2) in vec2 inOrigin;
layout(location = 3) in vec2 inSize;

layout(location = 0) out vec2 outUv;

void main() {
    outUv = inUv;

    gl_Position = vec4(inPosition * inSize + inOrigin, 0.0, 1.0);
}