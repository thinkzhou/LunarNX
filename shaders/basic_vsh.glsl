#version 460

// =============================================================================
// LunarNX Passthrough Vertex Shader (deko3d)
// Simply passes through position and texture coordinates for a full-screen quad
// =============================================================================

layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec2 aTexCoord;

layout (location = 0) out vec2 vTextureCoord;

void main() {
    gl_Position = vec4(aPosition, 1.0);
    vTextureCoord = aTexCoord;
}
