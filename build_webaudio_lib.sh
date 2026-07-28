#!/bin/bash
#
# build_webaudio_lib.sh — Build webaudio-node C++ sources as static lib
#
# Output: obj/libwebaudio.a — links into cart.wasm for full Web Audio support
#

set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

EMSDK_ROOT="$(cd ../emsdk && pwd)"
source "$EMSDK_ROOT/emsdk_env.sh" 2>/dev/null || true

WA_SRC="$HERE/../webaudio-node/src/wasm"

if [ ! -f "$WA_SRC/audio_graph_simple.cpp" ]; then
    echo "ERROR: webaudio-node sources not found at $WA_SRC"
    exit 1
fi

mkdir -p obj/webaudio

CXXFLAGS="-O2 -std=c++17 -msimd128 -msse -msse2 \
    -D__i386__ -DDR_MP3_FLOAT_OUTPUT -DDR_MP3_ONLY_SIMD \
    -Wno-narrowing -Wno-deprecated-declarations \
    -I$WA_SRC/.. -I$WA_SRC/../vendor \
    `# opusfile.h includes <opus_multistream.h> and <ogg/ogg.h>, which live in` \
    `# the vendored opus/ogg include dirs. webaudio-node's own` \
    `# scripts/build-unified-real.sh passes these; this script did not, so a` \
    `# from-scratch build failed with "opus_multistream.h: file not found".` \
    -I$WA_SRC/../vendor/opus/include -I$WA_SRC/../vendor/ogg/include"

echo "=== Compiling webaudio-node C++ sources ==="

# Core (skip webaudio.cpp — it's an old monolithic build, nodes/*.cpp is the modular version)
em++ $CXXFLAGS -c "$WA_SRC/audio_graph_simple.cpp" -o obj/webaudio/audio_graph_simple.o
em++ $CXXFLAGS -c "$WA_SRC/audio_decoders.cpp"     -o obj/webaudio/audio_decoders.o

# Utils
em++ $CXXFLAGS -c "$WA_SRC/utils/fft.cpp"          -o obj/webaudio/fft.o
em++ $CXXFLAGS -c "$WA_SRC/utils/audio_param.cpp"  -o obj/webaudio/audio_param.o 2>/dev/null || true
em++ $CXXFLAGS -c "$WA_SRC/utils/mixer.cpp"        -o obj/webaudio/mixer.o 2>/dev/null || true
em++ $CXXFLAGS -c "$WA_SRC/utils/resampler.cpp"    -o obj/webaudio/resampler.o 2>/dev/null || true

# Nodes
for f in oscillator_node gain_node buffer_source_node biquad_filter_node \
         delay_node wave_shaper_node stereo_panner_node constant_source_node \
         convolver_node dynamics_compressor_node analyser_node panner_node \
         iir_filter_node channel_splitter_node channel_merger_node; do
    em++ $CXXFLAGS -c "$WA_SRC/nodes/${f}.cpp" -o "obj/webaudio/${f}.o"
done

# Create static library
emar rcs obj/libwebaudio.a obj/webaudio/*.o

LIB_SIZE=$(wc -c < obj/libwebaudio.a)
echo "  obj/libwebaudio.a ($LIB_SIZE bytes, $(( LIB_SIZE / 1024 )) KB)"
echo "=== webaudio lib build complete ==="
