#version 450
layout(set = 0, binding = 0) uniform texture2D image;
layout(set = 0, binding = 1) uniform sampler filtering;
layout(push_constant) uniform Push { vec4 tint; } params;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    color = textureLod(sampler2D(image, filtering), uv, 0.0) * params.tint;
}
