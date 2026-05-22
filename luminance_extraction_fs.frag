#version 450

layout(set = 0, binding = 0) uniform sampler2D tex_sampler;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = vec2(vUV.x, vUV.y);
    vec4 color = texture(tex_sampler, uv);

    float alpha = color.a;
    float threshold = 0.8;
    float luminance = dot(color.rgb, vec3(0.299, 0.587, 0.114));

    color.rgb = step(threshold, luminance) * color.rgb;
    outColor = vec4(color.rgb, alpha);
}