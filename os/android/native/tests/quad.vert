#version 450
layout(push_constant) uniform PC { vec4 color; vec2 xrange; float z; } pc;
void main() {
  vec2 p[6] = vec2[](vec2(0,0), vec2(1,0), vec2(0,1), vec2(1,0), vec2(1,1), vec2(0,1));
  vec2 v = p[gl_VertexIndex];
  gl_Position = vec4(mix(pc.xrange.x, pc.xrange.y, v.x), v.y * 2.0 - 1.0, pc.z, 1.0);
}
