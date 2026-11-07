---
name: nxminiapp-ops
description: NXMiniApp（Switch 小程序容器）的工程与运维手册，含架构原理、构建打包、NRO 推送真机、后台管理 API、文档维护、远程部署与真机崩溃排查。当需要改动该仓库代码、构建或推送 Switch 容器、上传小程序包/容器包、更新文档、部署 ecfun.cc 服务器时使用。小程序业务代码开发见仓库内 skill/nxminiapp-dev。
---

# NXMiniApp 工程与运维

## 坐标

| 项 | 值 |
|---|---|
| 代码仓库 | `/Users/mr/Project/Switch/NXMiniApp` |
| USB 工具 | `/Users/mr/Project/Switch/Tools/mac/build/swlink-cli` |
| 线上站点 | `https://ecfun.cc`（部署目录 `/root/NXMiniApp`） |
| Switch 容器 | `sdmc:/switch/miniapp.nro` |
| Switch 配置 | `sdmc:/switch/nxkit/config.json` |
| 运行日志 | `sdmc:/switch/miniapp/miniapp.log` |
| 崩溃报告 | `sdmc:/atmosphere/crash_reports/` |

## 架构原理

三层，改任何一层都要想清楚另外两层：

- `client/`：borealis 原生渲染的容器。`apps/miniapp` 是容器本体（商店式启动器 + 小程序运行时），`core/` 是 NXKit 核心（HTTP/配置/媒体/UI 组件），`library/` 是 borealis、QuickJS、Yoga。
  小程序渲染链路：WXML → wxml_parser → Yoga 排版 → NanoVG 绘制，JS 由 QuickJS 常驻执行，**不经过 webview**。
- `server/`：Go + Gin + GORM + MySQL。`internal/api` 是对客户端开放的 `/api/v1/*`，`internal/admin` 是 `/api/v1/admin/*`（Bearer 鉴权），`internal/docs` 提供 DB 驱动的文档站。
- `docs/`：文档站源文件（静态 HTML）。首次启动时写入 MySQL，之后以后台 DB 内容为准。

容器启动流程：`main.cpp` → 落盘日志 → `nxkit::runApp` → `LauncherView`（读商店/最近使用）→ 进入某条小程序 → `MiniappView`。

## 小程序开发（业务代码）

组件/API 清单见 [references/miniapp-capabilities.md](references/miniapp-capabilities.md)，权威行为以仓库 `docs/` 为准。**不在这两份文档里的能力一律视为不存在，不要按微信官方文档推测。**

### 能力边界

页面用 WXML 描述结构、WXSS 子集描述样式，JS 由 QuickJS 常驻执行，经 Yoga 排版、NanoVG 原生绘制，**不经过 webview**。它是微信小程序的可运行子集：

- 没有自定义组件、canvas、map、web-view、npm 构建、微信开发者工具、WXS
- `video` 仅在启用 MPV 的构建上真实播放（Switch 默认 `NXKIT_WITH_MPV=ON`；桌面需装 libmpv）
- WXSS 不支持 `!important`、伪类、伪元素、复杂选择器组合

### 包结构

```
<package>/
├── app.json          # 必需：页面清单与窗口配置
├── app.js            # 可选：App({ onLaunch, globalData })
├── app.wxss          # 可选：全局样式
├── meta.json         # 仅商店目录用，不进安装包
├── icon.png          # 仅商店目录用，不进安装包
├── images/
└── pages/
    └── index/
        ├── index.wxml   # 必需
        ├── index.wxss   # 必需（可为空）
        └── index.js     # 必需，Page({...})
```

```json
{
  "pages": ["pages/index/index", "pages/detail/detail"],
  "window": { "backgroundColor": "#141518", "navigationBarTitleText": "我的小程序" }
}
```

`pages[0]` 是启动页；其余页用 `wx.navigateTo({ url: '/pages/detail/detail?id=7' })` 打开。

### 硬性规则（违反直接导致安装/运行失败）

1. 包根必须直接包含 `app.json`，zip 里不能再套一层目录（`hello/app.json` 会报「安装包缺少 app.json」）。
2. 每个页面必须同时存在 `.wxml` / `.wxss` / `.js`，`.wxss` 可为空但文件必须在。
3. 只用容器内置的 22 类标签，未知标签静默降级为 `view`。
4. 界面更新只能走 `this.setData()`，没有双向绑定、没有 WXS。
5. `app.json` 只有 `pages`、`window.backgroundColor`、`window.navigationBarTitleText` 生效；`tabBar`、`usingComponents`、`subPackages` 会被忽略。
6. 小程序 ID 必须匹配 `^[a-z0-9][a-z0-9_-]{1,63}$`。
7. 投稿限制：包 ≤ 32MB（.zip/.nxpkg），图标 png/jpg/webp ≤ 2MB。
8. 容器端直接安装要求 **ZIP_STORED（不压缩）**；投稿给服务端用普通 zip 即可（服务端下载时重打包为 stored）。

### 页面与 JS

- WXML：`{{ }}` 插值、`wx:if` / `wx:elif` / `wx:else`、`wx:for`（建议配 `wx:key`）、`bind*` 事件；`bindtap` 冒泡、`catchtap` 终止。
- WXSS：`rpx` 以 750 设计稿为基准；类选择器为主；页面样式叠加在 `app.wxss` 之上，内联 `style` 优先级最高。
- JS：每页独立 QuickJS 上下文，`Page({ data, onLoad(query), onShow, onReady, onHide, onUnload })`；支持 `async/await`、Promise、定时器。
- 参考实现：`client/resources/miniapps/playground/`（事件、表单、轮播、存储、路由全覆盖）。

### 本地预览与打包

```bash
# 桌面直接预览本地包（仅桌面构建支持）
NXKIT_MINIAPP_DEV=<包目录> ./client/build_desktop/apps/miniapp/miniapp

# 容器可读的 .nxpkg（必须 stored）
cd <包目录> && zip -X -0 -r ../myapp.nxpkg .
```

打包自检：解压第一层必须看到 `app.json`；`pages` 数组与实际文件路径完全一致。

### 发布

- 投稿页 `https://<host>/submit`（免登录，管理员审核）或 `POST /api/v1/submissions`（multipart，字段 `title id developer category version summary description icon package`）。
- 已在商店的小程序更新：`POST /api/v1/admin/miniapps/:id/package`（form `file` + `version`），或走灰度 `rollout/package` → `rollout/promote`。
- 内置示例包（如首页 `client/resources/miniapps/home`）改完要升 `meta.json` 的 `version`，且不能低于线上版本，否则不会热更。

### 常见错误

- 页面中央红色错误文字：容器不白屏，错误直接画在屏幕上，按文本对照 `docs/reference/errors.html`。
- 「安装包必须使用 ZIP_STORED」：用 `zip -X -0` 重打。
- 样式不生效：先确认用的是文档列出的 WXSS 子集属性与选择器。
- 调用了不存在的 `wx.*`：占位接口不报错也不做事，用 `wx.canIUse` 探测。
- 投稿返回 409 = ID 被占用，400 = app.json 缺失/类型或大小不符，429 = 每 IP 每小时限 10 次。

## 构建

Switch 真机（首次配置见 `README.md`）：

```bash
cd /Users/mr/Project/Switch/NXMiniApp/client
DEVKITPRO=/opt/devkitpro cmake --build build_switch --target miniapp.nro -j 8
```

产物：`client/build_switch/apps/miniapp/miniapp.nro`。

桌面预览：

```bash
cd /Users/mr/Project/Switch/NXMiniApp/client
cmake --build build_desktop --target miniapp -j 8
```

## 推送 NRO 到真机

### 换电脑时先拿到 swlink-cli

工具源码与使用文档在远端仓库，不在项目仓库里：

```bash
git clone git@github.com:xbw12138/SwitchTools.git
cd SwitchTools && cat README.md          # 协议、sysmodule 安装、使用教程
cd mac && ./setup.sh                     # 装依赖（brew + 精简 ffmpeg）并构建
# 产物：mac/build/swlink-cli
```

仓库内还带了两份可直接使用的资料：`.trae/skills/swlink-cli/SKILL.md`（CLI 用法 skill）、`docs/使用教程.md`（分步教程与排错表）。构建产物 `mac/build/` 被 gitignore，新机器必须自己编译一次。

**必须先让用户在 Switch 上按 B 完全退出容器**（只按 Home 挂起会锁住文件，报「设备错误 1/3」）。

```bash
CLI=/Users/mr/Project/Switch/Tools/mac/build/swlink-cli
$CLI put <本地 miniapp.nro> /switch/miniapp2.nro
$CLI rm /switch/miniapp.nro
$CLI mv /switch/miniapp2.nro /switch/miniapp.nro
$CLI get /switch/miniapp.nro /tmp/card.nro
shasum -a 256 /tmp/card.nro <本地 miniapp.nro>
```

**只能靠 sha256 确认版本**：不同版本的 NRO 大小经常完全相同（约 36.5MB），`ls` 看不出新旧。

## 读日志与崩溃排查

```bash
$CLI get /switch/miniapp/miniapp.log /tmp/miniapp.log          # 每次启动会截断
$CLI ls /atmosphere/crash_reports | grep "\.log" | tail -5
$CLI get /atmosphere/crash_reports/<name>.log /tmp/cr.log
```

崩溃报告关键字段：`Type`/`Address`（真正的出错访问地址）、`PC`、`ReturnAddress[NN]`、`Stack Region`。
符号化（**必须用产生该崩溃的同一次构建的 ELF**，否则地址全错位）：

```bash
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line \
  -e client/build_switch/apps/miniapp/miniapp.elf -f -C 0x1618e98
```

也可用 `aarch64-none-elf-objdump -d --start-address=... --stop-address=...` 看崩溃指令在做什么。

## 后台管理 API

鉴权：`Authorization: Bearer $NXKIT_ADMIN_TOKEN`（线上值为 `xbw12138`，配置在服务器 `.env`）。

| 用途 | 接口 |
|---|---|
| 发布容器版本 | `POST /api/v1/admin/releases`，form `file`(.nro) `version` `notes` `published`(默认 true) |
| 改发布状态 | `PATCH /api/v1/admin/releases/:id/flags`，JSON `{"published":true}` |
| 上传小程序包 | `POST /api/v1/admin/miniapps/:id/package`，form `file`(.nxpkg/.zip) `version` |
| 灰度包 | `POST /api/v1/admin/miniapps/:id/rollout/package`、`/rollout/promote`、`/rollout/discard` |
| 首页内容 | `GET/POST/PUT/DELETE /api/v1/admin/content` |
| 编辑文档页 | `PUT /api/v1/admin/docs/page`，`POST /api/v1/admin/docs/reset` |
| 导出文档 | `GET /api/v1/admin/docs/export` |
| 概览数据 | `GET /api/v1/admin/stats` |

客户端侧公开接口：`/api/v1/home`、`/api/v1/miniapps`、`/api/v1/miniapps/:id/download`、`POST /api/v1/miniapps/:id/open`（PV/UV）、`POST /api/v1/content/:id/open`（资讯曝光）、`/api/v1/container/releases`。

上传后建议本地先 `shasum -a 256` 与接口返回的 `sha256` 对照，确认传输无损。

## 文档维护

- 面向用户的文档在 `docs/`：`api/index.html`、`components/`、`style/`、`guide/`、`reference/`。
- **只要改了容器能力（新增 `wx.*`、组件属性、错误码、商店/上报接口），必须同步更新对应文档页**，这是项目既有约定。
- 文档站内容存 MySQL：仓库里的 HTML 只是首次写入的种子。线上改文档走后台文档页（`PUT /api/v1/admin/docs/page`），改仓库文件不会自动覆盖线上已存在的页。

## 远程部署

```bash
sshpass -p '<root 密码>' ssh root@ecfun.cc \
  "cd /root/NXMiniApp && git pull --ff-only && docker compose up -d --build"
```

- 后台前端（Vue/Vite）**只在 Docker 构建阶段编译**，本地没有 npm，不要尝试本地构建前端。
- 部署后用 `curl -s https://ecfun.cc/api/v1/health` 确认，并检查容器已 Started。
- 数据库迁移靠 GORM `AutoMigrate`，加字段/加表只需改 `server/internal/models` 与 `database.go`，重启即生效。

## 已知坑

1. **MPV 必须懒初始化**：启动路径上任何 `MPVCore::instance()` 都会触发 `mpv_initialize`（内置脚本加载等大量分配），在内存紧张时直接崩。`VideoView::mpv()` 才是首次初始化点；`isValid()` 不能依赖 `registered`，否则播放前会误报「需要启用 libmpv」。
2. **Switch 内存诊断的两个假信号**：`svcGetInfo(InfoType_UsedMemorySize)` 接近 Total 不代表 OOM；libnx 的 `sbrk` 不检查上限，探测出的可扩容量也不可信。判断依据应是崩溃报告的 `Type: Data Abort` + `Address` 是否落在未映射区，以及 `mallinfo()` 的 arena 增长。
3. **图片/视频解码失败不要逐帧重试**：每帧重读磁盘 + 重新分配会迅速打干堆，必须失败后锁定。
4. **小程序包**：容器本地安装要求 ZIP_STORED（`zip -X -0 -r`），投稿到服务端用普通 zip（服务端重打包）。
5. **NRO 文件被占用**：读可以，`rm`/`mv` 会失败，必须先退出应用。
6. 改 `client/resources/miniapps/home` 后要升 `meta.json` 的 version，否则服务端不会热更；注意本地版本号不要低于线上。
