#version 440
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in float weight;
layout(location=0) out vec2 texcoord;
layout(location=1) out float influence;
layout(std140,binding=0) uniform Params { mat4 matrix; vec4 options; };
void main(){gl_Position=matrix*vec4(position,1);texcoord=uv;influence=weight;}
