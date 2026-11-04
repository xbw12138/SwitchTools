#!/usr/bin/env bash
# swlink-cli 冒烟测试:覆盖连接、上传、下载、目录递归与校验和比对
#
# 用法: skills/swlink-cli/scripts/smoke-test.sh [swlink-cli 路径]
# 前置: Switch 已开机、USB 数据线直连、SwLink.app 已退出

set -euo pipefail

CLI=${1:-./mac/build/swlink-cli}
REMOTE_ROOT="/switch/.swl_smoke"

if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    sed -n '2,6p' "$0" | sed 's/^# *//'
    exit 0
fi

if [[ ! -x "$CLI" ]]; then
    echo "找不到可执行文件: $CLI" >&2
    echo "先构建: cd mac && cmake -S . -B build && cmake --build build -j4" >&2
    exit 1
fi

LOCAL=$(mktemp -d)
cleanup() {
    rm -rf "$LOCAL"
    "$CLI" rm "$REMOTE_ROOT" --dir >/dev/null 2>&1 || true
}
trap cleanup EXIT

sum() {
    if command -v md5 >/dev/null 2>&1; then
        md5 -q "$1"
    else
        md5sum "$1" | cut -d' ' -f1
    fi
}

step() { printf '\n== %s\n' "$*"; }

step "1/6 连接与容量"
"$CLI" df

step "2/6 上传 1B / 64KB / 5MB"
mkdir -p "$LOCAL/up"
printf 'x' > "$LOCAL/up/a.bin"
head -c 65536 /dev/urandom > "$LOCAL/up/b.bin"
head -c 5242880 /dev/urandom > "$LOCAL/up/c.bin"
"$CLI" mkdir "$REMOTE_ROOT"
"$CLI" put "$LOCAL/up" "$REMOTE_ROOT"

step "3/6 校验远端尺寸"
"$CLI" ls "$REMOTE_ROOT" | tee "$LOCAL/ls.txt"
grep -q " 5242880  c.bin" "$LOCAL/ls.txt" || { echo "c.bin 尺寸不符" >&2; exit 1; }
grep -q "   65536  b.bin" "$LOCAL/ls.txt" || { echo "b.bin 尺寸不符" >&2; exit 1; }

step "4/6 递归下载并比对校验和"
mkdir -p "$LOCAL/down"
"$CLI" get "$REMOTE_ROOT" "$LOCAL/down"
for f in a.bin b.bin; do
    [[ "$(sum "$LOCAL/up/$f")" == "$(sum "$LOCAL/down/$f")" ]] || { echo "$f 校验和不一致" >&2; exit 1; }
done
[[ "$(sum "$LOCAL/up/c.bin")" == "$(sum "$LOCAL/down/c.bin")" ]] || { echo "c.bin 校验和不一致" >&2; exit 1; }

step "5/6 重命名"
"$CLI" mv "$REMOTE_ROOT/a.bin" "$REMOTE_ROOT/a2.bin"
"$CLI" ls "$REMOTE_ROOT" | grep -q "a2.bin" || { echo "重命名失败" >&2; exit 1; }

step "6/6 清理"
"$CLI" rm "$REMOTE_ROOT" --dir
"$CLI" ls /switch | grep -q ".swl_smoke" && { echo "远端目录未清理" >&2; exit 1; }

printf '\nPASS: 上传/下载/目录递归/重命名/清理全部正常\n'
