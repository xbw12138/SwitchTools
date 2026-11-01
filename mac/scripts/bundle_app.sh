#!/bin/bash
# 把 .app 变成可双击运行的自包含包:拷贝非系统依赖库、改写 install name、加图标、本地签名
# 用法: bundle_app.sh <AppBundle 路径> <可执行文件路径> <图标 icns 路径>
set -e

APP="$1"
BIN="$2"
ICON="$3"

FRAMEWORKS="$APP/Contents/Frameworks"
RESOURCES="$APP/Contents/Resources"

mkdir -p "$FRAMEWORKS" "$RESOURCES"

# 拷贝 AppIcon.icns
if [ -f "$ICON" ]; then
    cp -f "$ICON" "$RESOURCES/AppIcon.icns"
fi

# 递归拷贝非系统 dylib,并把引用改写成 @rpath/<name>
copy_deps() {
    local target="$1"

    # 已处理过的跳过
    if [ -f "$FRAMEWORKS/.done_$(basename "$target")" ]; then
        return 0
    fi
    touch "$FRAMEWORKS/.done_$(basename "$target")"

    otool -L "$target" 2>/dev/null | tail -n +2 | awk '{print $1}' | while read -r dep; do
        [ -z "$dep" ] && continue
        case "$dep" in
            /usr/lib/*|/System/*|@rpath/*|@executable_path/*) continue ;;
        esac

        local base
        base="$(basename "$dep")"
        local dest="$FRAMEWORKS/$base"

        if [ ! -f "$dest" ]; then
            cp -L "$dep" "$dest"
            chmod u+w "$dest"
            install_name_tool -id "@rpath/$base" "$dest" 2>/dev/null || true
        fi

        install_name_tool -change "$dep" "@rpath/$base" "$target" 2>/dev/null || true
    done

    # 依赖库自己引用的库也要拷进来(例如 ffmpeg 各库互相依赖)
    otool -L "$target" 2>/dev/null | tail -n +2 | awk '{print $1}' | while read -r dep; do
        [ -z "$dep" ] && continue
        case "$dep" in
            /usr/lib/*|/System/*|@rpath/*|@executable_path/*) continue ;;
        esac
        local base
        base="$(basename "$dep")"
        local dest="$FRAMEWORKS/$base"
        if [ -f "$dest" ] && [ ! -f "$FRAMEWORKS/.done_$base" ]; then
            copy_deps "$dest"
        fi
    done
}

copy_deps "$BIN"

# 让可执行文件从包内 Frameworks 找库
if ! otool -l "$BIN" | grep -A2 LC_RPATH | grep -q "@executable_path/../Frameworks"; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$BIN" 2>/dev/null || true
fi

# 清理标记文件
rm -f "$FRAMEWORKS"/.done_*

# Homebrew 的库改过 install name 后签名失效,统一做本地临时签名
codesign --force --deep --timestamp=none --sign - "$APP" >/dev/null 2>&1 || true

echo "已打包: $APP"
