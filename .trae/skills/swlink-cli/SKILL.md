---
name: swlink-cli
description: 通过 USB 读写 Switch SD 卡的命令行工具 swlink-cli 的使用教程，含设备端安装（商店一键装或手动装 sysmodule）、上传下载、目录递归传输、NRO 推送、Wi-Fi 串流与 USB 排障。当需要命令行批量操作 Switch SD 卡文件、推送 NRO、安装 SwLink sysmodule 或排查 16C0:27DA 连接问题时使用。
---

# swlink-cli 使用教程

`swlink-cli` 与 SwLink.app 共用同一套 USB 协议，直接读写 Switch 的 SD 卡，无需局域网、无需进入 hbmenu。

配套资源（与本文件同目录；仓库内位于 `skills/swlink-cli/`）：

- `scripts/smoke-test.sh` — 一键冒烟测试
- `references/protocol.md` — USB 文件协议与固件实现约束
- `references/troubleshooting.md` — 完整排障手册

## 前置条件

1. 设备端已部署 SwLink sysmodule（title id `00FF0000A53B5A57`，`/atmosphere/contents/00FF0000A53B5A57/exefs.nsp`），且主机已开机
   - 最省事：Switch 上打开 NXMiniApp 容器 → 应用商店 → **SwLink** → 安装 → 重启主机；之后可一键升级
   - 手动装：解压 `dist/swlink-sysmodule-sd.zip` 到 SD 卡根目录（含 `exefs.nsp`、`toolbox.json`、`flags/boot2.flag`）
   - 当前版本 **1.2.0**：新增局域网 SD 卡文件通道（TCP 12347）；修复 HOS 23.0.1 下 NPDM 被拒加载、锁屏/休眠唤醒后监听线程空转卡死
2. USB-C **数据线**直插 Mac 机身口，不经过 hub / 扩展坞 / 显示器
3. Switch 屏幕亮着、未休眠
4. **SwLink.app 必须已退出**——USB 接口独占，App 在运行会报 `claim_interface: Access denied`

连接自检：

```sh
system_profiler SPUSBDataType | grep -A6 SwLink
```

应看到 `SwLink Switch`、`Vendor ID: 0x16c0`、`Product ID: 0x27da`、`Serial Number: swlink-01`。

## Wi-Fi 与 USB 的分工

- **Wi-Fi 串流**：SwLink.app 里点「无线连接」，走 TCP `12345`（UDP `12346` 自动发现），
  Switch 与 Mac 需在同一局域网，推荐 5GHz
- **Wi-Fi 文件**：1.2.0 起设备端多监听一个 TCP `12347`，App 的文件页会按需另开连接，
  与串流互不干扰；大文件仍建议插 USB
- **CLI 只走 USB**：`swlink-cli` 没有 `--wifi` 参数，SD 卡读写必须插线
- 串流会话与文件接口互不阻塞，但同一个 USB 设备不能被 App 和 CLI 同时占用

## 构建

```sh
cd mac
cmake -S . -B build
cmake --build build -j4          # 产物: mac/build/swlink-cli
```

只编 CLI 可加 `--target swlink-cli`。项目已内置 imgui 源码，构建不需要联网。

## 命令

```sh
./mac/build/swlink-cli <命令> [参数]
```

| 命令 | 说明 |
|---|---|
| `ls [路径]` | 列出目录，默认 `/` |
| `get <远端> <本地>` | 下载文件；远端是目录时递归下载到本地目录 |
| `put <本地> [远端目录或路径]` | 上传文件或目录（递归）。目标是**已存在目录**才拼本地文件名，否则当作完整目标路径 |
| `nro <本地.nro> [远端路径]` | 写入 NRO，默认 `switch/<文件名>` |
| `mkdir <远端目录>` | 建目录，已存在视为成功 |
| `rm <远端> [--dir]` | 删除文件；`--dir` 递归删除目录 |
| `mv <远端旧> <远端新>` | 重命名/移动 |
| `df` | 显示 SD 卡总容量与可用空间 |

退出码 `0` 成功，`1` 失败；进度条输出到 stderr，结果输出到 stdout。

## 路径规则

- 远端路径一律以 `/` 开头（`/` 即 SD 卡根），禁止相对路径
- 不以 `/` 开头时 CLI 会自动补上，末尾多余的 `/` 会被去掉
- `put` 目标判定依赖设备端 `stat`：目录存在 → `/dir/<本地名>`；否则整个字符串就是目标路径
  - `put /tmp/a.txt /switch` → `/switch/a.txt`
  - `put /tmp/a.txt /switch/renamed.txt` → `/switch/renamed.txt`
- `get` 本地参数若是已存在目录，自动拼远端文件名；否则按完整本地路径写

## 典型用法

```sh
# 看容量和目录
./mac/build/swlink-cli df
./mac/build/swlink-cli ls /switch

# 推送一个 NRO(之后在 Switch 上打开 hbmenu 即可看到)
./mac/build/swlink-cli nro ./hbmenu.nro

# 上传整个目录到新目录,再递归拉回来核对
./mac/build/swlink-cli put ./payload /switch/payload
./mac/build/swlink-cli get /switch/payload ./downloaded

# 清理
./mac/build/swlink-cli rm /switch/payload --dir
```

## 冒烟验证

改了客户端或固件后跑一次：

```sh
bash scripts/smoke-test.sh            # 默认用 ./mac/build/swlink-cli
bash scripts/smoke-test.sh <cli 路径>
```

脚本会依次验证：连接与容量 → 上传 1B/64KB/5MB → 远端尺寸 → 递归下载与校验和比对 → 重命名 → 清理，全部通过时打印 `PASS`。

手工快速验证：

```sh
./mac/build/swlink-cli df
head -c 5242880 /dev/urandom > /tmp/t.bin
./mac/build/swlink-cli put /tmp/t.bin /switch/t.bin        # 应到 100%
./mac/build/swlink-cli ls /switch | grep t.bin             # 尺寸应为 5242880
./mac/build/swlink-cli get /switch/t.bin /tmp/t2.bin       # 应到 100%
md5 -q /tmp/t.bin /tmp/t2.bin                              # 两行必须相同
./mac/build/swlink-cli rm /switch/t.bin
```

## 常见问题速查

| 现象 | 处理 |
|---|---|
| `未找到 Switch 设备 16C0:27DA` | 数据线/端口/sysmodule 未就绪，先 `system_profiler` 自检；必要时换机身口或重启主机 |
| `claim_interface: Access denied`、`0xe00002d8` | SwLink.app 仍占用，退出后重试 |
| `设备错误 3` | 多为远端父路径不存在（文件路径被当目录拼接）；用完整目标路径 |
| `设备错误 2` | 路径不存在 |
| 下载得到 0 字节 | 旧固件 `Stat` 对齐填充导致解析错位，升级设备端 nsp |
| 传输后所有命令都不通 | 端点被打废，拔插 USB 线或重启主机 |
| 锁屏唤醒后设备消失/连不上 | 唤醒后监听 socket 会失效，设备端 200ms 后自动重建；等 1~2 秒重试，日志出现 `TCP accept 异常 errno` 后跟 `TCP 监听` 即正常 |
| 开机后 SD 卡上没有 `SwLink.log` | sysmodule 未加载（多因 NPDM 能力声明被固件拒绝）；换 1.1.0 及以后的包 |

细节与逐步排查见 `references/troubleshooting.md`。

## 注意

- 设备端文件接口与串流会话独立，CLI **不握手**；握手会把设备标记为"已连接"，客户端退出后设备无从感知，下次可能收不到 HELLO
- 不要在传输过程中切换页面/退出 App/拔线，半截请求会让端点停滞数秒
- 删改远端文件前先 `ls` 确认；`rm --dir` 递归删除不可恢复
- 单块写入上限与 4KB 对齐等固件侧约束见 `skills/swlink-cli/references/protocol.md`
