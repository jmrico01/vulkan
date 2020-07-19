#version 450

// Vertex attributes
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;

// Instance attributes
layout(location = 3) in mat4 model; // mat4 takes locations 3, 4, 5, 6
layout(location = 7) in vec3 color;
layout(location = 8) in vec3 collapseMid;
layout(location = 9) in float collapseT;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outColor;

layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
} ubo;

// TODO copied in lightmapMesh.frag
vec3 lerp(vec3 a, vec3 b, float t)
{
	return a * (1.0 - t) + b * t;
}

float random(vec2 uv)
{
	return fract(sin(dot(uv, vec2(12.9898, 78.233))) * 43758.5453123);
}

void main()
{
	float randMagMax = 0.2;
	float randMagMin = 0.05;

	vec4 worldNormal = model * vec4(inNormal, 0.0);
    outNormal = normalize(worldNormal.xyz);
    outColor = inColor * color;

	float randOffset = 0.0;
	if (collapseT > 0.0) {
		vec2 seedUv = vec2(inPosition.x * inPosition.z * collapseT, inPosition.y * inPosition.z * collapseT);
		float rand = random(seedUv);
		float randMag = max((1.0 - collapseT) * randMagMax, randMagMin);
		randOffset = 2.0 * randMag * (rand - 0.5);
	}

	vec3 collapsedPos = lerp(inPosition, collapseMid, collapseT + randOffset);

	vec4 pos = ubo.proj * ubo.view * model * vec4(collapsedPos, 1.0);
    gl_Position = pos;
}
