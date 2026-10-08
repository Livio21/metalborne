#version 450
layout(location = 0) in FlatInput { flat vec2 uv; } data;
layout(location = 0) out vec4 color;
void main() { color = vec4(data.uv, 0.0, 1.0); }
