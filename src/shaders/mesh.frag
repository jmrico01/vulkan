#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 outColor;

vec3 lerp(vec3 a, vec3 b, float t)
{
	return a * (1.0 - t) + b * t;
}

void main()
{
	float gamma = 2.2;
	vec3 colorGammaCorrected = pow(inColor, vec3(gamma));

	// outColor = vec4(colorGammaCorrected, 1.0);
	outColor = vec4(1.0, 1.0, 1.0, 1.0);
}
