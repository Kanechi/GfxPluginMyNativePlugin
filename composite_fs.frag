#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_smapler;
layout(set = 0, binding = 1) uniform sampler2D blur_sampler;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    vec2 uv = vec2(vUV.x, 1.0 -vUV.y);

    vec4 sceneColor = texture(scene_smapler, uv);
    vec4 bloomColor = texture(blur_sampler, uv);

    float bloomIntensity = 2.0;
    outColor = vec4(sceneColor.rgb + bloomColor.rgb * bloomIntensity, sceneColor.a);
}