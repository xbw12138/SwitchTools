#!/bin/sh
# SwLink macOS 客户端依赖安装 + 构建
# 用法: ./setup.sh
set -e
cd "$(dirname "$0")"

# 1. Homebrew
if ! command -v brew >/dev/null 2>&1; then
    echo "未检测到 Homebrew,正在安装..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
fi

# 2. 依赖 (ffmpeg 不通过 brew: 受 macOS 版本限制可能没有预编译包,
#    源码编译还会连带上 Python 等重型依赖, 故改为下面第 3 步自行编译精简版)
brew install cmake sdl2 libusb pkg-config ninja

# 3. 内置精简 ffmpeg (仅 h264 解码器, 装到 third_party/ffmpeg)
#    已存在则跳过, 需要重编时 rm -rf third_party/ffmpeg 再跑本脚本
if [ ! -f third_party/ffmpeg/lib/libavcodec.dylib ]; then
    echo "编译内置 ffmpeg (约需数分钟)..."
    BUILD_DIR=/tmp/swlink-ffmpeg-build
    mkdir -p "$BUILD_DIR"
    if [ ! -d "$BUILD_DIR/ffmpeg-7.1" ]; then
        curl -sL -o "$BUILD_DIR/ffmpeg.tar.xz" https://ffmpeg.org/releases/ffmpeg-7.1.tar.xz
        tar -xf "$BUILD_DIR/ffmpeg.tar.xz" -C "$BUILD_DIR"
    fi
    cd "$BUILD_DIR/ffmpeg-7.1"
    ./configure --prefix="$OLDPWD/third_party/ffmpeg" \
        --enable-shared --disable-static --disable-programs --disable-doc \
        --disable-x86asm --disable-everything \
        --enable-decoder=h264 --enable-parser=h264 \
        > "$BUILD_DIR/configure.log" 2>&1
    make -j"$(sysctl -n hw.ncpu)" > "$BUILD_DIR/make.log" 2>&1
    make install > "$BUILD_DIR/install.log" 2>&1
    cd "$OLDPWD"
fi

# 4. 构建
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

echo ""
echo "构建完成: $PWD/build/swlink"
echo "Switch 端 sysmodule 需用 devkitPro 构建,见 README.md"
