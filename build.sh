#!/bin/bash

# HunterClaw Build Script

set -e

echo "====================================="
echo "  HunterClaw Build Script"
echo "====================================="
echo ""

# Check if required dependencies are installed
echo "Checking dependencies..."

check_dependency() {
    if ! command -v $1 &> /dev/null; then
        echo "❌ $1 is not installed"
        echo "   Install with: $2"
        exit 1
    else
        echo "✅ $1 found"
    fi
}

check_dependency cmake "sudo apt-get install cmake"
check_dependency g++ "sudo apt-get install g++"

echo ""
echo "Checking libraries..."

# Check for pkg-config
if command -v pkg-config &> /dev/null; then
    echo "✅ pkg-config found"
    
    # Check libraries
    for lib in yaml-cpp libcurl openssl; do
        if pkg-config --exists $lib 2>/dev/null; then
            echo "✅ $lib found"
        else
            echo "⚠️  $lib may not be installed"
        fi
    done
fi

echo ""
echo "Enabling devtoolset-11 for C++17 support..."
source /opt/rh/devtoolset-11/enable || echo "Warning: devtoolset-11 not available, using system compiler"

echo ""
echo "Creating build directory..."
mkdir -p build
cd build

echo "Running CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release

echo ""
echo "Building HunterClaw..."
make -j$(nproc)

echo ""
echo "====================================="
echo "✅ Build completed successfully!"
echo "====================================="
echo ""
echo "To run HunterClaw:"
echo "  ./build/hunterclaw-gateway"
echo ""
echo "To install system-wide:"
echo "  sudo make install"
echo ""
