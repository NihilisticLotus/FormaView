#version 440
layout(location=0) in vec2 texcoord;
layout(location=1) in float influence;
layout(location=2) in vec3 worldNormal;
layout(location=3) in vec3 surfaceTint;
layout(location=4) in vec3 surfaceCoverage;
layout(location=5) in vec3 deformedPosition;
layout(location=6) in vec4 vertexColour;
layout(location=0) out vec4 color;
layout(std140,binding=0) uniform Params { mat4 matrix; vec4 options; };
layout(binding=1) uniform sampler2D sourceTexture;
vec3 linearValue(vec3 v){return mix(v/12.92,pow((v+.055)/1.055,vec3(2.4)),step(vec3(.04045),v));}
vec3 displayValue(vec3 v){return mix(v*12.92,1.055*pow(max(v,vec3(0)),vec3(1.0/2.4))-.055,step(vec3(.0031308),v));}
// Warm key and cool fill match the inspection stage. Keep analytical colour,
// checker, heat and wire displays independent of the studio light colour.
vec3 studioLight(vec3 normal) {
    vec3 n = normalize(normal);
    if (options.y < 1.5)
        return vec3(.3 + .7 * abs(dot(n, normalize(vec3(.4,.7,1)))));
    float key = max(dot(n, normalize(vec3(6,10,7))), 0.0);
    float fill = max(dot(n, normalize(vec3(-5,4,-5))), 0.0);
    vec3 sky = mix(vec3(.28,.27,.25), vec3(.48,.51,.55), n.y * .5 + .5);
    return sky + key * vec3(.57,.55,.51) + fill * vec3(.24,.28,.34);
}
void main(){
    if(surfaceCoverage.y < -2.5) {
        vec3 distanceToEdge = surfaceTint / max(fwidth(surfaceTint), vec3(1e-5));
        float edge = min(distanceToEdge.x, min(distanceToEdge.y, distanceToEdge.z));
        if (edge > 1.1) discard;
        color = vec4(mix(vec3(.07,.48,.56), vec3(.15,.91,.94), surfaceCoverage.x),
                     surfaceCoverage.x * (1.0 - smoothstep(.45, 1.1, edge)));
        return;
    }
    if(surfaceCoverage.y < -1.5) { color=vec4(surfaceTint,1); return; }
    if(options.w>1.5) {
        if(options.w>4.5) {
            vec3 n=dot(worldNormal,worldNormal)>1e-20?worldNormal:cross(dFdx(deformedPosition),dFdy(deformedPosition));
            if(dot(n,n)<1e-20)n=vec3(0,0,1);
            vec3 light=studioLight(n);
            color=vec4(vec3(.847,.843,.820)*light,1);
            return;
        }
        if(options.w>3.5) { color=vec4(.85,.88,.92,1); return; }
        if(options.w>2.5) { color=vec4(vertexColour.rgb,1); return; }
        vec3 n=dot(worldNormal,worldNormal)>1e-20?worldNormal:cross(dFdx(deformedPosition),dFdy(deformedPosition));
        if(dot(n,n)<1e-20)n=vec3(0,0,1);
        vec3 a=abs(n);
        vec2 spatial=a.x>a.y && a.x>a.z ? deformedPosition.yz : a.y>a.z ? deformedPosition.xz : deformedPosition.xy;
        vec2 p=vertexColour.w>.5 ? texcoord*16 : spatial*12;
        vec2 footprint=max(fwidth(p),vec2(1e-5));
        vec2 wave=abs(fract((p-.5*footprint)*.5)*2-1)-abs(fract((p+.5*footprint)*.5)*2-1);
        float checker=.5-.5*(wave.x/footprint.x)*(wave.y/footprint.y);
        float light=.3+.7*abs(dot(normalize(n),normalize(vec3(.4,.7,1))));
        color=vec4(vec3(mix(.22,.78,checker)*light),1);
        return;
    }
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
        vec3 n=dot(worldNormal,worldNormal)>1e-20?worldNormal:cross(dFdx(deformedPosition),dFdy(deformedPosition));
        if(dot(n,n)<1e-20)n=vec3(0,0,1);
        vec3 light=studioLight(n);
        if(options.z>0.5)color.rgb=displayValue(linearValue(color.rgb)*surfaceTint*light);
        else color.rgb*=surfaceTint*light;
    }
}
