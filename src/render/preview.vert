#version 440
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in float weight;
layout(location=3) in vec3 normal;
layout(location=4) in vec3 tint;
layout(location=5) in vec3 coverage;
layout(location=0) out vec2 texcoord;
layout(location=1) out float influence;
layout(location=2) out vec3 worldNormal;
layout(location=3) out vec3 surfaceTint;
layout(location=4) out vec3 surfaceCoverage;
layout(std140,binding=0) uniform Params { mat4 matrix; vec4 options; };
void main(){gl_Position=matrix*vec4(position,1);texcoord=uv;influence=weight;worldNormal=normal;surfaceTint=tint;surfaceCoverage=coverage;}
