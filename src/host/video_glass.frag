#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 panelSize;
    float marginPx;
} u;
layout(binding = 1) uniform sampler2D source;

void main() {
    vec2 pixel = qt_TexCoord0 * u.panelSize;
    vec2 uv = (pixel + u.marginPx) / (u.panelSize + 2.0 * u.marginPx);
    vec3 backdrop = texture(source, uv).rgb;
    float luma = dot(backdrop, vec3(0.2126, 0.7152, 0.0722));
    vec3 tint = vec3(0.102, 0.109, 0.127);
    vec3 material = tint + mix(vec3(luma), backdrop, 1.12) * 0.37;
    material += 0.034 * (1.0 - qt_TexCoord0.y);

    float radius = min(22.0, min(u.panelSize.x, u.panelSize.y) * 0.5);
    vec2 box = abs(pixel - u.panelSize * 0.5) - u.panelSize * 0.5 + radius;
    float edge = length(max(box, 0.0)) + min(max(box.x, box.y), 0.0) - radius;
    float aa = max(fwidth(edge), 0.001);
    float coverage = 1.0 - smoothstep(-0.5 * aa, 0.5 * aa, edge);
    float highlight = smoothstep(-1.7 * aa, -0.35 * aa, edge);
    material += highlight * mix(0.22, 0.075, qt_TexCoord0.y);
    float alpha = coverage * u.qt_Opacity;
    fragColor = vec4(material * alpha, alpha);
}
