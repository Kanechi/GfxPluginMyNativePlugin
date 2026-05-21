#version 450

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
	vec2 uv = vec2(vUV.x, 1.0 - vUV.y);
	outColor = texture(uTex, uv);
}