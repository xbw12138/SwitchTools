# SwLink

USB 线或同一 Wi-Fi 连接 Switch(大气层 Atmosphere)与 macOS,实现 **游戏画面+音频串流** 和 **SD 卡文件传输**。

全自研两端,不依赖 SysDVR:

```
┌─────────────── Switch (sysmodule) ───────────────┐        ┌──── macOS ────┐
│ grc:d ──> capture.c ──> stream_server.c ─┐       │  USB   │  swlink 客户端 │
│  H.264 720p / PCM 48kHz                  ├─usb:ds├◄══════►│  SDL2+FFmpeg   │
│ SD 卡 <── fs IPC ── file_server.c ───────┘ 2 接口 │        │  +ImGui       │
│                                          └─TCP───┼◄─Wi-Fi─►│  (net_link)   │
└─────────────────────────────────────────────────┘        └───────────────┘
```

- **接口 0(串流)**:OUT 握手 / IN 复用视频+音频包流(18B 头 + payload)
- **接口 1(文件)**:40B 请求/响应头 + payload,8 条命令(PING/LIST/STAT/READ/WRITE/MKDIR/DELETE/RENAME)
- **无线**:TCP `12345` 复用同一套握手与包格式;UDP `12346` 负责局域网发现,SD 卡文件传输仍走 USB
- 协议契约见 [protocol/swlink_protocol.h](protocol/swlink_protocol.h),USB VID:PID = `16C0:27DA`

> 只想用、不想看代码?直接看 **[分步使用教程](docs/使用教程.md)**(含"一次成功"检查清单和按现象排错表)。

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

**不想编译 sysmodule**:直接下载现成的 SD 包,解压到 SD 卡根目录即可,目录结构已建好:

```sh
curl -LO https://raw.githubusercontent.com/xbw12138/SwitchTools/main/dist/swlink-sysmodule-sd.zip
unzip swlink-sysmodule-sd.zip -d /Volumes/SWITCH    # 换成你的 SD 卡挂载点
```

包里含 `atmosphere/contents/00FF0000A53B5A57/{exefs.nsp,toolbox.json,flags/boot2.flag}` 与一份 `README-sysmodule.txt`。

重启主机,或在 Tesla 菜单的 Sysmodules 里启用。

## 二、macOS 客户端构建

```sh
cd mac && ./setup.sh     # 装 brew 依赖(cmake/sdl2/libusb/pkg-config) + 编译内置精简 ffmpeg
open build/SwLink.app    # 双击 App 即可运行,无需命令行
```

> 构建产物是自包含的 `mac/build/SwLink.app`(依赖库与图标已打进包内,已做本地临时签名)。
> 首次打开若被系统拦截,右键 → 打开即可。

> ffmpeg 不通过 Homebrew 安装:旧版 macOS(如 12)上 brew 没有预编译包,源码编译还会
> 连带 Python 等重型依赖。`setup.sh` 会自行下载 ffmpeg 7.1 并只编译 h264 解码器,
> 安装到 `mac/third_party/ffmpeg`(不入库)。依赖装好后日常只需
> `cmake --build build -j`。

## 三、使用

1. Switch 开机(确保 SwLink sysmodule 已启动)
2. 连接方式二选一:
   - **USB**:USB 线连 Mac,点 **USB 连接**
   - **Wi-Fi**:Switch 与 Mac 连同一个 Wi-Fi,点 **无线连接**,选自动发现的设备(或手填 IP)后连接;
     全程无需插线,建议 5GHz
3. 进游戏即出画面和声音;「独立窗口」可把纯画面弹到单独窗口(可缩放到副屏/外接屏),
   打开后主窗口停止渲染画面
4. 「SD 卡文件」页可浏览 SD 卡、多选/递归下载、上传(支持整目录拖入)、新建/重命名/批量删除;
   点表头排序,传输中显示速度、剩余时间并可取消,底部显示 SD 剩余容量(文件功能仅 USB 模式可用)
5. 把本地文件或文件夹直接拖进窗口,即可上传到当前目录
6. 「NRO 推送」页把本地 `.nro` 经 USB 写入 SD 卡(默认 `switch/`),之后在 Switch 上
   按住 R 启动 hbmenu,进 `switch` 目录点选即可运行

### 命令行(swlink-cli)

图形界面之外还提供一个命令行工具,共用同一套 USB 协议,适合脚本/批量操作:

**免编译直接用**(macOS x86_64,已内置 libusb,Gatekeeper 首次运行需放行):

```sh
curl -LO https://raw.githubusercontent.com/xbw12138/SwitchTools/main/dist/swlink-cli-macos-x64.zip
unzip swlink-cli-macos-x64.zip -d swlink-cli
xattr -dr com.apple.quarantine swlink-cli
./swlink-cli/swlink-cli ls /switch
```

两个文件必须放在同一目录(`swlink-cli` 通过 `@executable_path` 找 `libusb-1.0.0.dylib`)。Apple Silicon 上需已安装 Rosetta。

**从源码构建**:

```sh
cd mac && cmake --build build -j        # 产物:mac/build/swlink-cli
./build/swlink-cli --help

./build/swlink-cli ls /switch                  # 列目录
./build/swlink-cli nro ./hbmenu.nro            # 推送到 /switch/hbmenu.nro
./build/swlink-cli put ./payload /switch       # 上传文件或整个目录(递归)
./build/swlink-cli get /switch/logs ./logs     # 下载文件;目录则递归下载
./build/swlink-cli mkdir /switch/new
./build/swlink-cli rm /switch/old --dir
./build/swlink-cli df                          # SD 卡容量
```

> swlink-cli 与 SwLink.app **不能同时占用设备**,使用前先退出 App(USB 接口是独占的)。

## 四、限制与注意事项

- **只有支持系统录屏(长按截图键)的游戏才能采集**。不支持的游戏和主菜单 `grc:d` 会无限阻塞(无画面,属正常现象)。建议安装 [dvr-patches](https://github.com/exelix11/dvr-patches) 解锁全部游戏
- **不要与 SysDVR 同时运行**:usb:ds 为独占设备,两个 sysmodule 会冲突
- **无线串流**:720p30 码流约 4–10 Mbps,5GHz 稳定;2.4GHz 或信号差时可能掉帧/重连。
  主机会在链路中断后提示断开,重新点连接即可;无线模式不支持 SD 卡文件传输
- 视频为 grc 硬编 720p H.264,延迟比 RTSP/网络方案略低但不等于零
- 文件传输走文件接口,与串流互不阻塞;大文件按 512KB 分块
- **换另一台 Switch**:固件同为 18.1.0 时直接复制 SD 卡上的 3 个文件即可;固件版本不同需按该版本同期 libnx 重编(否则模块静默不加载),详见[教程 2.2 节](docs/使用教程.md)
- 系统版本要求 5.0+(仅实现 usb:ds 新接口)

## 目录结构

```
protocol/swlink_protocol.h   两端唯一协议契约
switch/                      sysmodule(devkitPro 工程)
  source/usb_link.c          usb:ds 双接口初始化与端点读写
  source/capture.c           grc:d 采集(H.264+PCM,SPS/PPS 注入)
  source/stream_server.c     握手 + 流发送(视频 main 线程,音频独立线程,USB/TCP 共用)
  source/net_server.c        无线接入:TCP 串流监听 + UDP 设备发现
  source/file_server.c       SD 卡文件服务(原生 fs IPC,无堆)
  source/main.c              入口,2MB 静态堆,拉起各线程
  Makefile / swlink.json     构建配置(title id 00FF0000A53B5A57)
mac/                         macOS 客户端(CMake 工程)
  src/link.hpp               传输层抽象接口(StreamClient/FileClient 只依赖它)
  src/usb_link.cpp           libusb 连接/握手/端点读写
  src/net_link.cpp           Wi-Fi:TCP 连接、局域网发现,复用同一套包协议
  src/stream_client.cpp      流拆包线程
  src/video_decoder.cpp      FFmpeg H.264 解码 -> SDL YUV 纹理
  src/audio_output.cpp       SDL 音频队列(积压 0.5s 丢旧,支持静音)
  src/file_client.cpp        文件传输协议客户端
  src/main.cpp               ImGui 界面(串流/文件/帮助 三页)
  macos/Info.plist           App 包元信息
  macos/AppIcon.icns         应用图标
  scripts/bundle_app.sh      把依赖库打进 .app 并做本地签名
```
