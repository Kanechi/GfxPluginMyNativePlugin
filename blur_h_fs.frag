#version 450

layout(set = 0, binding = 0) uniform sampler2D tex_sampler;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
	vec2 uv = vec2(vUV.x, vUV.y);

	ivec2 sz = textureSize(tex_sampler, 0);
	float width = float(sz.x);

	const float sigma = 3.0;
	const int halfKernel = 3;

	vec4 sum = vec4(0.0);
	float total = 0.0;

	for(int x = -halfKernel; x <= halfKernel; ++x)
	{
        float w = exp(-(x * x) / (2.0 * sigma * sigma));
        vec2 offset = vec2(float(x) / width, 0.0);
        sum += texture(tex_sampler, uv + offset) * w;
        total += w;
	}

	outColor = sum / total;
}