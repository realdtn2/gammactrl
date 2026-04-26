#!/bin/bash
# Exit immediately if any command fails
set -e

echo "Configuring build directory..."
cmake -B build

echo "Compiling gammactrl..."
cmake --build build -j$(nproc)

echo "Installing gammactrl to ~/.local..."
cmake --install build --prefix ~/.local

echo "Build and Installation complete! You can now launch Gamma Control from your application menu."
