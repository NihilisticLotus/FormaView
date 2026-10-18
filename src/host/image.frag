#version 440
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(std140,binding=0) uniform Params { mat4 correction; vec4 rectangle; vec4 viewport; vec4 colour; vec4 alpha; };
layout(binding=1) uniform sampler2D source;
vec3 oetf(vec3 c) { return mix(1.055 * pow(max(c, vec3(0)), vec3(1.0 / 2.4)) - .055, 12.92 * c, lessThanEqual(c, vec3(.0031308))); }
float eotf(float c) { return c <= .04045 ? c / 12.92 : pow((c + .055) / 1.055, 2.4); }
void main() {
    vec2 position = uv * viewport.xy;
    vec2 tc = (position - rectangle.xy) / rectangle.zw;
    if (viewport.z < .5 && (any(lessThan(tc, vec2(0))) || any(greaterThanEqual(tc, vec2(1))))) { result = vec4(.1333, .1373, .1490, 1); return; }
    vec4 c = texture(source, tc);
    int channel = int(viewport.w);
    if (channel >= 0) c = vec4(vec3(c[channel]), 1);
    else {
        if (colour.w > 2.5 && c.a != 0) c.rgb /= c.a;
        if (colour.y > .5) c.rgb *= exp2(colour.x);
    }
    if (any(isnan(c)) || any(isinf(c))) { result = vec4(1, 0, 1, 1); return; }
    if (channel < 0 && alpha.x > .5 && colour.w > 1.5) {
        float checker = mod(floor(position.x / 12) + floor(position.y / 12), 2) == 0 ? .27 : .34;
        if (alpha.y > 2.5) checker = .5;
        else if (alpha.y > 1.5) checker = 1;
        else if (alpha.y > .5) checker = 0;
        if (colour.y > .5 && colour.z > .5) checker = eotf(checker);
        c.rgb = mix(vec3(checker), c.rgb, clamp(c.a, 0, 1));
    }
    if (channel < 0 && colour.y > .5 && colour.z > .5) c.rgb = oetf(c.rgb);
    result = vec4(clamp(c.rgb, 0, 1), 1);
}
