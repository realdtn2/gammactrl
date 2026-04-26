#!/bin/bash
# Exit immediately if any command fails
set -e

echo "Configuring build directory..."
cmake -B build

echo "Compiling gammactrl..."
# Use all available CPU cores for a faster build
cmake --build build -j$(nproc)

echo "Build complete!"
