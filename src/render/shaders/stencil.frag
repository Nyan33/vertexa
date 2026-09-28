#version 440
// Only the stencil buffer is written (colour writes are masked).
layout(location = 0) out vec4 fragColor;
void main() { fragColor = vec4(0.0); }
