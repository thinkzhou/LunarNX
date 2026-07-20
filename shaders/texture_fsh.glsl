#version 460

// =============================================================================
// LunarNX NV12→RGB Fragment Shader (deko3d)
// Reference: Moonlight-Switch texture_fsh.glsl
//
// Input:  Y plane  (R8_Unorm,  binding 0)
//         UV plane (RG8_Unorm, binding 1) — interleaved CbCr
//
// Conversion: YUV → RGB using BT.709 matrix via uniform
// =============================================================================

layout (location = 0) in vec2 vTextureCoord;
layout (location = 0) out vec4 outColor;

layout (binding = 0) uniform sampler2D plane0;  // Y
layout (binding = 1) uniform sampler2D plane1;  // CbCr (interleaved)

layout (std140, binding = 0) uniform Transformation {
    mat3 yuvmat;
    vec3 offset;
    vec4 uv_data;
} u;

void main() {
    // Apply viewport offset/scale for aspect ratio correction
    vec2 uv = (vTextureCoord - u.uv_data.xy) * u.uv_data.zw;

    // Sample Y (plane0.r) and CbCr (plane1.rg)
    vec3 yuv = vec3(
        texture(plane0, uv).r,
        texture(plane1, uv).r,
        texture(plane1, uv).g
    ) - u.offset;

    vec3 rgb = u.yuvmat * yuv;
    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
