#version 440
layout(location = 0) in vec2 glassUv;
layout(location = 0) out vec4 color;
layout(std140, binding = 0) uniform GlassUniforms {
    mat4 matrix;
    float opacity;
    vec4 frameBounds;
    vec4 panel;
} uniforms;
layout(binding = 1) uniform sampler2D videoFrame;

vec3 sampleFrame(vec2 pixel) {
    vec2 uv = uniforms.frameBounds.xy + pixel * uniforms.frameBounds.zw / uniforms.panel.xy;
    vec3 background = vec3(0.063, 0.067, 0.078);
    vec2 inside = step(vec2(0.0), uv) * step(uv, vec2(1.0));
    return mix(background, texture(videoFrame, clamp(uv, 0.0, 1.0)).rgb, inside.x * inside.y);
}

void main() {
    vec2 size = uniforms.panel.xy;
    float radius = min(uniforms.panel.z, min(size.x, size.y) * 0.5);
    vec2 pixel = glassUv * size;
    vec2 centered = pixel - size * 0.5;
    vec2 box = abs(centered) - size * 0.5 + radius;
    float distance = length(max(box, 0.0)) + min(max(box.x, box.y), 0.0) - radius;
    float coverage = 1.0 - smoothstep(-1.0, 0.0, distance);
    float rim = 1.0 - smoothstep(0.0, 1.5, abs(distance));
    float falloff = exp(min(distance, 0.0) * 0.095);
    vec2 towardCenter = -centered / max(length(centered), 1.0);
    vec2 refracted = clamp(pixel + 12.0 * falloff * towardCenter, vec2(0.0), size);
    vec3 source = sampleFrame(refracted) * 0.36
        + 0.16 * (sampleFrame(refracted + vec2(5.0, 2.0))
                + sampleFrame(refracted - vec2(5.0, 2.0))
                + sampleFrame(refracted + vec2(2.0, -4.0))
                + sampleFrame(refracted - vec2(2.0, -4.0)));
    vec3 glass = mix(source, vec3(0.095, 0.102, 0.123), 0.59);
    glass += vec3(0.043 * falloff + rim * mix(0.21, 0.094, glassUv.y));
    float alpha = coverage * uniforms.opacity;
    color = vec4(glass * alpha, alpha);
}
