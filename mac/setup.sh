#!/bin/sh
# SwLink macOS 客户端依赖安装 + 构建
# 用法: ./setup.sh
set -e

# 1. Homebrew
if ! command -v brew >/dev/null 2>&1; then
    echo "未检测到 Homebrew,正在安装..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

# 2. 依赖
brew install cmake sdl2 ffmpeg libusb pkg-config

# 3. 构建
cd "$(dirname "$0")"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

echo ""
echo "构建完成: $(dirname "$0")/build/swlink"
echo "Switch 端 sysmodule 需用 devkitPro 构建,见 README.md"
