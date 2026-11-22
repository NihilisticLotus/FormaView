#version 440
layout(location = 0) in vec4 position;
layout(location = 1) in vec2 coordinates;
layout(location = 0) out vec2 glassUv;
layout(std140, binding = 0) uniform GlassUniforms {
    mat4 matrix;
    float opacity;
    vec4 frameBounds;
    vec4 panel;
} uniforms;
void main() {
    glassUv = coordinates;
    gl_Position = uniforms.matrix * position;
}
