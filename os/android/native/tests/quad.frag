#version 450
layout(push_constant) uniform PC { vec4 color; vec2 xrange; float z; } pc;
layout(location = 0) out vec4 o;
void main() { o = pc.color; }
