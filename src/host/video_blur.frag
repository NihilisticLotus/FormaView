#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform Params {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 sampleStep;
} u;
layout(binding = 1) uniform sampler2D source;

void main() {
    // Normalized Gaussian, sigma = 4 reduced texels, truncated at 3 sigma.
    vec4 sum = texture(source, qt_TexCoord0);
    float total = 1.0;
    for (int i = 1; i <= 12; ++i) {
        float w = exp(-float(i * i) / 32.0);
        vec2 offset = float(i) * u.sampleStep;
        sum += w * (texture(source, qt_TexCoord0 + offset) + texture(source, qt_TexCoord0 - offset));
        total += 2.0 * w;
    }
    fragColor = sum * (u.qt_Opacity / total);
}
