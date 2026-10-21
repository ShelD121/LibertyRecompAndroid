#!/bin/bash
# Runs the dynamic rendering emulation (../dynamic_rendering.cpp) on a Linux
# Vulkan driver with the Khronos validation layer, the emulation forced on.
# Needs: g++, glslangValidator, the Vulkan loader and headers, a Vulkan driver
# (lavapipe works) and the validation layer. On Ubuntu:
#   apt-get install g++ glslang-tools libvulkan-dev mesa-vulkan-drivers vulkan-validationlayers
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
out="${OUT:-$(mktemp -d)}"
glslangValidator -V "$here/quad.vert" --vn quad_vert -o "$out/quad_vert.h" >/dev/null
glslangValidator -V "$here/quad.frag" --vn quad_frag -o "$out/quad_frag.h" >/dev/null
g++ -std=c++17 -O1 -I"$out" -I"$here/host_stub" -I"$here/.." \
  "$here/dynamic_rendering_test.cpp" "$here/../dynamic_rendering.cpp" -lvulkan \
  -o "$out/dynamic_rendering_test"
LIBERTY_EMULATE_DYNAMIC_RENDERING=1 "$out/dynamic_rendering_test"
