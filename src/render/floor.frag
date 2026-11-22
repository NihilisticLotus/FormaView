#version 440
layout(location=0) in vec2 floorPosition;
layout(location=0) out vec4 color;

void main() {
    float opacity = 0.22 * (1.0 - smoothstep(6.0, 60.0, length(floorPosition)));
    color = vec4(0.39, 0.42, 0.45, opacity);
}
