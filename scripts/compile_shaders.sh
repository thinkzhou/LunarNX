#!/bin/bash
# Compile GLSL shaders to deko3d .dksh binaries using uam
# Prerequisites: devkitPro with switch-dev installed (provides uam)
# Usage: ./scripts/compile_shaders.sh

set -e

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
UAM="${DEVKITPRO}/tools/bin/uam"
SHADER_DIR="$(cd "$(dirname "$0")/../shaders" && pwd)"
OUTPUT_DIR="$(cd "$(dirname "$0")/../romfs/shaders" && pwd)"

if [ ! -f "$UAM" ]; then
    echo "Error: uam not found at $UAM"
    echo "Install devkitPro and switch-dev: sudo dkp-pacman -S switch-dev"
    exit 1
fi

echo "Compiling shaders with uam..."
mkdir -p "$OUTPUT_DIR"

# Fragment shader: NV12→RGB
echo "  texture_fsh.glsl → texture_fsh.dksh"
"$UAM" -s frag -o "$OUTPUT_DIR/texture_fsh.dksh" "$SHADER_DIR/texture_fsh.glsl"

# Vertex shader: passthrough
echo "  basic_vsh.glsl → basic_vsh.dksh"
"$UAM" -s vert -o "$OUTPUT_DIR/basic_vsh.dksh" "$SHADER_DIR/basic_vsh.glsl"

# Fragment shader: RGBA post-process final blit
echo "  upscaling_pass_fsh.glsl → upscaling_pass_fsh.dksh"
"$UAM" -s frag -o "$OUTPUT_DIR/upscaling_pass_fsh.dksh" "$SHADER_DIR/upscaling_pass_fsh.glsl"

# Fragment shader: EASU upscaling
echo "  upscaling_fsh.glsl → upscaling_fsh.dksh"
"$UAM" -s frag -o "$OUTPUT_DIR/upscaling_fsh.dksh" "$SHADER_DIR/upscaling_fsh.glsl"

# Fragment shader: RCAS sharpening
echo "  rcas_fsh.glsl → rcas_fsh.dksh"
"$UAM" -s frag -o "$OUTPUT_DIR/rcas_fsh.dksh" "$SHADER_DIR/rcas_fsh.glsl"

echo "Done. Shader binaries written to $OUTPUT_DIR"
