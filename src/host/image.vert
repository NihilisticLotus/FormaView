#version 440
layout(location=0) out vec2 uv;
layout(std140,binding=0) uniform Params { mat4 correction; vec4 rectangle; vec4 viewport; vec4 colour; vec4 alpha; };
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    uv = p;
    gl_Position = correction * vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0, 1);
}
