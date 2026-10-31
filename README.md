# SwLink

USB 连接 Switch(大气层 Atmosphere)与 macOS,实现 **游戏画面+音频串流** 和 **SD 卡文件传输**。

全自研两端,不依赖 SysDVR:

```
┌─────────────── Switch (sysmodule) ───────────────┐        ┌──── macOS ────┐
│ grc:d ──> capture.c ──> stream_server.c ─┐       │  USB   │  swlink 客户端 │
│  H.264 720p / PCM 48kHz                  ├─usb:ds├◄══════►│  SDL2+FFmpeg   │
│ SD 卡 <── fs IPC ── file_server.c ───────┘ 2 接口 │        │  +ImGui       │
└─────────────────────────────────────────────────┘        └───────────────┘
```

- **接口 0(串流)**:OUT 握手 / IN 复用视频+音频包流(18B 头 + payload)
- **接口 1(文件)**:40B 请求/响应头 + payload,8 条命令(PING/LIST/STAT/READ/WRITE/MKDIR/DELETE/RENAME)
- 协议契约见 [protocol/swlink_protocol.h](protocol/swlink_protocol.h),USB VID:PID = `16C0:27DA`

## 一、Switch 端(sysmodule)构建与安装

需要 devkitPro + libnx:

```sh
# macOS 安装 devkitPro
curl -L https://github.com/devkitPro/pacman/releases/latest/download/devkitpro-pacman-installer.pkg -o dkp.pkg
sudo installer -pkg dkp.pkg -target /
export DEVKITPRO=/opt/devkitpro
sudo dkp-pacman -S switch-dev   # libnx 等

# 构建(输出 swlink.nsp)
cd switch && make
```

安装到 SD 卡:

```
/atmosphere/contents/00FF0000A53B5A57/exefs.nsp      <- 由 swlink.nsp 改名
/atmosphere/contents/00FF0000A53B5A57/flags/boot2.flag  <- 空文件,开机自启
/atmosphere/contents/00FF0000A53B5A57/toolbox.json   <- 复制 switch/toolbox.json
```

重启主机,或在 Tesla 菜单的 Sysmodules 里启用。

## 二、macOS 客户端构建

```sh
cd mac && ./setup.sh     # 装 brew 依赖(cmake/sdl2/libusb/pkg-config) + 编译内置精简 ffmpeg
./build/swlink
```

> ffmpeg 不通过 Homebrew 安装:旧版 macOS(如 12)上 brew 没有预编译包,源码编译还会
> 连带 Python 等重型依赖。`setup.sh` 会自行下载 ffmpeg 7.1 并只编译 h264 解码器,
> 安装到 `mac/third_party/ffmpeg`(不入库)。依赖装好后日常只需
> `cmake --build build -j`。

## 三、使用

1. Switch 开机(确保 SwLink sysmodule 已启动),USB 线连 Mac
2. 打开 swlink 客户端,「串流」页签点 **连接 Switch**
3. 进游戏即出画面和声音;「文件」页签可浏览 SD 卡、上传/下载/删除/新建/重命名

## 四、限制与注意事项

- **只有支持系统录屏(长按截图键)的游戏才能采集**。不支持的游戏和主菜单 `grc:d` 会无限阻塞(无画面,属正常现象)。建议安装 [dvr-patches](https://github.com/exelix11/dvr-patches) 解锁全部游戏
- **不要与 SysDVR 同时运行**:usb:ds 为独占设备,两个 sysmodule 会冲突
- 视频为 grc 硬编 720p H.264,延迟比 RTSP/网络方案略低但不等于零
- 文件传输走文件接口,与串流互不阻塞;大文件按 512KB 分块
- 系统版本要求 5.0+(仅实现 usb:ds 新接口)

## 目录结构

```
protocol/swlink_protocol.h   两端唯一协议契约
switch/                      sysmodule(devkitPro 工程)
  source/usb_comms.c         usb:ds 双接口初始化与端点读写
  source/capture.c           grc:d 采集(H.264+PCM,SPS/PPS 注入)
  source/stream_server.c     握手 + 流发送(视频 main 线程,音频独立线程)
  source/file_server.c       SD 卡文件服务(原生 fs IPC,无堆)
  source/main.c              入口,禁堆,线程拉起
  Makefile / swlink.json     构建配置(title id 00FF0000A53B5A57)
mac/                         macOS 客户端(CMake 工程)
  src/usb_link.cpp           libusb 连接/握手/端点读写
  src/stream_client.cpp      流拆包线程
  src/video_decoder.cpp      FFmpeg H.264 解码 -> SDL YUV 纹理
  src/audio_output.cpp       SDL 音频队列(积压 0.5s 丢旧)
  src/file_client.cpp        文件传输协议客户端
  src/main.cpp               ImGui 界面(串流/文件两页签)
```
