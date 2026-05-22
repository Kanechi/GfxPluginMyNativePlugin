#version 450

layout(set = 0, binding = 0) uniform sampler2D sceneTex;
layout(set = 0, binding = 1) uniform sampler2D bloomTex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = vec2(vUV.x, 1.0 - vUV.y);

    vec4 scene = texture(sceneTex, uv);
    vec4 bloom = texture(bloomTex, uv);

    float intensity = 1.0;
    outColor = vec4(scene.rgb + bloom.rgb * intensity, scene.a);
}