#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec4 outColor;

void main()
{
	vec3 lightColor = vec3(1.0, 1.0, 1.0);
	vec3 lightDir1 = normalize(vec3(0.4, 0.8, -1.0));
	vec3 lightDir2 = normalize(vec3(-0.6, -0.1, 1.0));

	vec3 negNormal = -inNormal;
	float dot1 = max(0.0, dot(negNormal, lightDir1));
	float dot2 = max(0.0, dot(negNormal, lightDir2));
	vec3 color = lightColor * dot1 + lightColor * dot2;
	color = clamp(color, vec3(0.0, 0.0, 0.0), vec3(1.0, 1.0, 1.0));

	float gamma = 2.2;
	vec3 colorGammaCorrected = pow(inColor * color, vec3(gamma));

	outColor = vec4(colorGammaCorrected, 1.0);
}
