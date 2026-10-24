#version 440
layout(location=0) in vec3 position;
layout(location=1) in vec2 uv;
layout(location=2) in float weight;
layout(location=3) in vec3 normal;
layout(location=4) in vec3 tint;
layout(location=5) in vec3 coverage;
layout(location=6) in uvec2 skinRange;
layout(location=7) in uvec2 morphRange;
layout(location=0) out vec2 texcoord;
layout(location=1) out float influence;
layout(location=2) out vec3 worldNormal;
layout(location=3) out vec3 surfaceTint;
layout(location=4) out vec3 surfaceCoverage;
layout(location=5) out vec3 deformedPosition;
layout(std140,binding=0) uniform Params { mat4 matrix; vec4 options; mat4 display; vec4 skinOptions; vec4 skinMode; };
layout(binding=3) uniform sampler2D influenceTexture;
layout(binding=4) uniform sampler2D paletteTexture;
layout(binding=5) uniform sampler2D morphTexture;
vec4 fetchPalette(uint at) { uint width=uint(skinOptions.y); return texelFetch(paletteTexture,ivec2(at%width,at/width),0); }
mat4 cluster(uint index) { uint at=index*4u; return transpose(mat4(fetchPalette(at),fetchPalette(at+1u),fetchPalette(at+2u),fetchPalette(at+3u))); }
void main() {
    // Animated morph targets (glTF weights channels): apply weighted local
    // deltas before the skin matrix. skinMode.y = palette texel base of the
    // group rows, .z = row stride, .w = morph texture width (0 = none).
    vec3 morphedPosition=position; vec3 morphedNormal=normal;
    if (skinMode.y>0.5 && skinMode.w>0.5) {
        uint width=uint(skinMode.w);
        uint count=morphRange.y&0xffffu; uint group=morphRange.y>>16u;
        for (uint t=0u;t<count;++t) {
            uint at=morphRange.x+t*2u;
            vec4 dp=texelFetch(morphTexture,ivec2(at%width,at/width),0);
            vec4 dn=texelFetch(morphTexture,ivec2((at+1u)%width,(at+1u)/width),0);
            float w=fetchPalette(uint(skinMode.y)+group*uint(skinMode.z)+t).x;
            morphedPosition+=dp.xyz*w; morphedNormal+=dn.xyz*w;
        }
    }
    mat4 blended=mat4(0); float sum=0;
    for (uint i=0u;i<skinRange.y;++i) {
        uint slot=skinRange.x+i; uint at=slot/2u; uint width=uint(skinOptions.x);
        vec4 pair=texelFetch(influenceTexture,ivec2(at%width,at/width),0);
        vec2 entry=(slot%2u)==0u?pair.xy:pair.zw;
        blended+=cluster(uint(entry.x))*entry.y; sum+=entry.y;
    }
    blended/=max(sum,1e-30);
    vec4 p=display*blended*vec4(morphedPosition,1);
    deformedPosition=p.xyz;
    mat3 linear=mat3(blended);
    mat3 cofactor=mat3(cross(linear[1],linear[2]),cross(linear[2],linear[0]),cross(linear[0],linear[1]));
    float det=dot(linear[0],cofactor[0]);
    // skinMode.x: ActorX reference normals rotate by the weighted bone matrices.
    vec3 n=skinMode.x>0.5 ? mat3(display)*(linear*morphedNormal) : abs(det)>1e-20 ? mat3(display)*(cofactor*morphedNormal/det) : vec3(0);
    worldNormal=length(n)>1e-12?normalize(n):vec3(0);
    texcoord=uv; influence=weight; surfaceTint=tint; surfaceCoverage=coverage;
    gl_Position=matrix*p;
    // Diagnostic rasterization uses this SAME skin shader and math. Each
    // selected source corner writes one exact position/normal float pixel.
    if (skinOptions.z>0.5) { gl_Position=vec4((float(gl_VertexIndex)+.5)/skinOptions.w*2-1,0,0,1); if(skinOptions.z>1.5)deformedPosition=worldNormal; }
}
