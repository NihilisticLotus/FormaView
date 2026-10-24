#version 440
// Keep the complete product varying interface. D3D11 assigns packed registers
// at shader compile time; a sparse PS interface shifts TEXCOORD5 into register0.
layout(location=0) in vec2 texcoord;
layout(location=1) in float influence;
layout(location=2) in vec3 worldNormal;
layout(location=3) in vec3 surfaceTint;
layout(location=4) in vec3 surfaceCoverage;
layout(location=5) in vec3 deformedPosition;
layout(location=0) out vec4 color;
void main() { color=vec4(deformedPosition,1); }
