# NXMiniApp 小程序能力速查

完整属性与行为以仓库 `docs/` 为准（组件 `docs/components/index.html`，API `docs/api/index.html`，样式 `docs/style/index.html`，排错 `docs/reference/errors.html`）。本表用于快速判断「能不能做」，不确定时必须回文档确认。

## 内置标签（22 类，常用项）

| 标签 | 默认尺寸 | 可聚焦 | 常用事件 |
|---|---|---|---|
| `view` | 自适应 | 否 | `bindtap` |
| `scroll-view` | 自适应 | 子节点 | `bindtap` |
| `text` | 按文本测量 | 否 | `bindtap` |
| `rich-text` | 宽 100% | 否 | — |
| `image` | 200×120rpx | 否 | `bindtap` |
| `icon` | 48×48rpx | 否 | — |
| `button` | 320×88rpx | 是 | `bindtap` |
| `navigator` | 100%×80rpx | 是 | `bindtap` + 路由 |
| `input` | 100%×80rpx | 是 | `bindinput` / `bindconfirm` |
| `textarea` | 100%×160rpx | 是 | `bindinput` / `bindconfirm` |
| `switch` | 96×56rpx | 是 | `bindchange` |
| `checkbox` / `radio` | 48×48rpx | 是 | `bindchange` |
| `slider` | 100%×80rpx | 是 | `bindchange` |
| `progress` | 100%×24rpx | 否 | — |
| `swiper` | 100%×300rpx | 自动轮播 | — |
| `swiper-item` | 100%×100% | 否 | — |
| `video` | 100%×360rpx | 是 | `bindplay` / `bindpause` / `bindended` / `bindtimeupdate` / `binderror` |

未知标签静默降级为 `view`；没有自定义组件。

## wx.* API（36 个真实可用）

- **网络**：`wx.request`（GET/POST/PUT/DELETE，回调 `success({data,statusCode,header})`）、`wx.getNetworkType`
- **界面**：`wx.showToast`、`wx.showLoading` / `wx.hideLoading`、`wx.showModal`（`success({confirm,cancel})`）、`wx.setNavigationBarTitle`、`wx.nextTick`、`wx.vibrateShort`（空实现）、`wx.canIUse`
- **视频**：`wx.createVideoContext(id)` → `play()` / `pause()` / `stop()` / `seek(seconds)`
- **存储**（按小程序隔离）：`wx.setStorageSync` / `getStorageSync` / `removeStorageSync` / `clearStorageSync` 及异步回调版；`wx.getStorageInfoSync()` → `{keys,currentSize,limitSize}`
- **路由**：`wx.navigateTo`（url 可带 query）、`wx.navigateBack({delta})`、`wx.redirectTo`、`wx.reLaunch`
- **系统**：`wx.getSystemInfoSync` / `getSystemInfo`、`getWindowInfo`、`getDeviceInfo`、`getAppBaseInfo`、`getMenuButtonBoundingClientRect`（零矩形）、`getLaunchOptionsSync` / `getEnterOptionsSync`
- **其他**：`wx.setClipboardData` / `getClipboardData`、`wx.getUpdateManager`（空实现）
- **容器扩展**：`wx.miniapp.deviceId()`（容器持久化设备标识，用于服务端 PV/UV 统计）

另有 18 个兼容占位接口：调用不报错也没有行为，便于从微信迁移。用 `wx.canIUse('schema')` 探测。

## 页面生命周期

`onLoad(query)` → `onShow()` → 首次绘制后 `onReady()`；压栈时 `onHide()`，出栈时 `onUnload()`。

## 页面模板

```js
// pages/index/index.js
Page({
  data: { msg: 'Hello' },
  onLoad(query) {},
  change() { this.setData({ msg: 'NXMiniApp' }); }
});
```

```html
<!-- pages/index/index.wxml -->
<view class="page">
  <text>{{ msg }}</text>
  <button bindtap="change">点我</button>
</view>
```

## 打包

| 场景 | 要求 | 命令 |
|---|---|---|
| 投稿到服务端 | 普通 zip（服务端重打包为 stored） | `zip -r app.zip .` |
| 容器直接读取（.nxpkg / 本地预览） | **ZIP_STORED、无数据描述符** | `cd pkg && zip -X -0 -r ../app.nxpkg .` |

约束：包根直接含 `app.json`；不允许绝对路径与 `..`；`meta.json` 与 `icon.png` 只用于商店目录，不进安装包。

## 商店接口（公开）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/health` | 健康检查 |
| GET | `/api/v1/miniapps` | 列表（仅已发布），支持 `q`、`category` |
| GET | `/api/v1/miniapps/{id}` | 详情 |
| GET | `/api/v1/miniapps/{id}/download` | 下载 `.nxpkg`（始终 ZIP_STORED） |
| GET | `/api/v1/miniapps/{id}/icon` | 封面（可能 404） |
| POST | `/api/v1/check-updates` | 批量版本检查 |
| POST | `/api/v1/submissions` | 免登录投稿（multipart） |
| POST | `/api/v1/miniapps/{id}/open` | 启动上报（PV/UV，body 可带 `device_id`） |
| GET | `/api/v1/container/releases` | 容器 NRO 版本列表（仅已发布，首个为最新） |

## 上架字段（meta.json / 投稿表单）

| 字段 | 默认 | 说明 |
|---|---|---|
| `id` | 目录名 | `^[a-z0-9][a-z0-9_-]{1,63}$` |
| `title` | 目录名 | 卡片标题（投稿必填） |
| `summary` | 空 | 一句话简介，参与搜索 |
| `description` | 空 | 详情描述 |
| `category` | 示例 | 工具 / 娱乐 / 教育 / 生活 / 游戏 / 示例 / 其他 |
| `developer` | 空 | 开发者 |
| `version` | 1.0.0 | 与本地 `.version` 比较决定是否重新下载 |
| `published` | true | false 时不出现在商店列表 |
