#version 440
layout(location=0) in vec3 guidePosition;
layout(location=0) out vec2 floorPosition;
layout(std140,binding=0) uniform Params { mat4 matrix; vec4 options; };

void main() {
    const vec2 corners[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(-1,1),
                                    vec2(-1,1), vec2(1,-1), vec2(1,1));
    floorPosition = corners[gl_VertexIndex] * 64.0;
    gl_Position = matrix * vec4(floorPosition.x, guidePosition.y, floorPosition.y, 1);
}
