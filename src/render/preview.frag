#version 440
layout(location=0) in vec2 texcoord;
layout(location=1) in float influence;
layout(location=0) out vec4 color;
layout(std140,binding=0) uniform Params { mat4 matrix; vec4 options; };
layout(binding=1) uniform sampler2D sourceTexture;
void main(){
    if(options.x<=0.5) color=texture(sourceTexture,texcoord);
    else if(isnan(influence)) color=vec4(.3,.3,.3,1);
    else if(influence<0 || influence>1) color=vec4(1,0,1,1);
    else color=vec4(influence,0,1-influence,1);
}
