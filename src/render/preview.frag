#version 440
layout(location=0) in vec2 texcoord;
layout(location=1) in float influence;
layout(location=2) in vec3 worldNormal;
layout(location=3) in vec3 surfaceTint;
layout(location=4) in vec3 surfaceCoverage;
layout(location=0) out vec4 color;
layout(std140,binding=0) uniform Params { mat4 matrix; vec4 options; };
layout(binding=1) uniform sampler2D sourceTexture;
vec3 linearValue(vec3 v){return mix(v/12.92,pow((v+.055)/1.055,vec3(2.4)),step(vec3(.04045),v));}
vec3 displayValue(vec3 v){return mix(v*12.92,1.055*pow(max(v,vec3(0)),vec3(1.0/2.4))-.055,step(vec3(.0031308),v));}
void main(){
    if(surfaceCoverage.y < -1.5) { color=vec4(surfaceTint,1); return; }
    if(options.w>0.5) {
        color=surfaceCoverage.y<0 ? vec4(surfaceTint,1) : vec4(texture(sourceTexture,texcoord).rgb,1);
        return;
    }
    if(options.x<=0.5) color=texture(sourceTexture,texcoord);
    else if(isnan(influence)) color=vec4(.3,.3,.3,1);
    else if(influence<0 || influence>1) color=vec4(1,0,1,1);
    else color=vec4(influence,0,1-influence,1);
    if(options.y>0.5 && options.x<=0.5) {
        color.a*=surfaceCoverage.x;
        if(surfaceCoverage.y>0.5 && surfaceCoverage.y<1.5 && color.a<surfaceCoverage.z) discard;
        if(surfaceCoverage.y<1.5)color.a=1;
        float light=.3+.7*abs(dot(normalize(worldNormal),normalize(vec3(.4,.7,1))));
        if(options.z>0.5)color.rgb=displayValue(linearValue(color.rgb)*surfaceTint*light);
        else color.rgb*=surfaceTint*light;
    }
}
