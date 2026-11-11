#!/usr/bin/env bash
# Builds Bathos and bathos-cli on Linux (cmake, a C++17 compiler, libsdl2-dev). ONNX Runtime is fetched once.
set -e
cd "$(dirname "$0")"
ORT=onnxruntime-linux-x64-1.20.1
if [ ! -f external/onnxruntime/libonnxruntime.so ]; then
    echo "Getting ONNX Runtime, once..."
    mkdir -p external/onnxruntime
    curl -sL "https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/$ORT.tgz" | tar -xz -C external
    cp -L external/$ORT/lib/libonnxruntime.so* external/onnxruntime/
    ln -sf "$(ls external/onnxruntime | grep 'so\.[0-9]' | head -1)" external/onnxruntime/libonnxruntime.so 2>/dev/null || true
    rm -rf external/$ORT
fi
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cp -L external/onnxruntime/libonnxruntime.so* build/
echo "Built: build/Bathos and build/bathos-cli (run them with LD_LIBRARY_PATH=build)"
