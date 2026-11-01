# 排查记录：自定义 sysmodule 在 Atmosphere 上"完全不加载"

> 时间：2026-11-01　主机：Switch（固件 18.1.0 / Atmosphere 1.7.1 / Hekate 6.2.0，文件型 emuMMC）
> 结论先说：**根因是 devkitPro 的 libnx 版本太新（2026-02 的 master），NSO 在 18.1.0 上静默启动失败。降到 libnx v4.12.0 重编后一次成功。**

## 1. 现象

自研 sysmodule `SwLink`（tid `00FF0000A53B5A57`）安装后：

- SD 根目录**始终没有** `/SwLink.log`（模块 main 的第一行就是写日志）
- `atmosphere/crash_reports` / `fatal_reports` 里**没有**新增记录
- Tesla 的 `Sysmodules` 覆盖层里 SwLink 显示"未启用"
- **连官方 SysDVR 也显示"未启用"**（这条后来成为关键交叉证据）
- 同卡上的 `ovlloader`（Tesla 本体，2024 年的构建）正常运行，Tesla 能打开

即：能跑的都是 2024 年构建的 sysmodule，跑不了的都是新工具链构建的。

## 2. 已排除的项（都验证过，别再重复折腾）

| 怀疑点 | 验证方式 | 结果 |
|---|---|---|
| Atmosphere 引擎文件损坏 | `shasum -a 256` 对比官方 1.7.1（package3 / stratosphere.romfs / hbl.nsp） | **完全相同** |
| 引导层没打签名补丁 | 读 `bootloader/hekate_ipl.ini` | 三个条目都有 `fss0=atmosphere/package3` + `kip1patch=nosigchk` |
| Hekate 版本不匹配 | 从 `payload.bin` 提取版本串 | Hekate 6.2.0，与 AMS 1.7.1 同期，兼容 |
| NSP 结构不合法 | 解析 PFS0 + 逐字节对比官方 SysDVR 的 `main.npdm` | 仅模块名与签名区 3 字节不同，**完全等价**（包括 4KB 主线程栈，SysDVR 也是 4KB） |
| `boot2.flag` / `toolbox.json` 缺失或错误 | 直接 `ls -la` | 都在，与 SysDVR 格式一致 |
| 目录名大小写 | 对比 tid 目录名 | 正确 |
| SD 写入权限 | 在 Mac 上直接写文件 | 正常 |

**教训**：`main_thread_stack_size` 4KB 看着像问题，但官方 SysDVR 的 NPDM 也是 4KB —— 不要凭直觉改，要拿能跑的同类比。

## 3. 定位过程（决定性的一步）

1. 用 SysDVR 源码确认其模式选择是"文件存在性"驱动（见附录），排除"设备找不到"的干扰。
2. 在 Tesla 里发现 **SysDVR 也未启用**，而 Tesla 自身（`ovlloader`，2024 年构建）正常 → 把问题从"我们的代码"上升到"运行环境 + 构建版本"。
3. 检查工具链：`ls -la /opt/devkitpro/libnx/lib/libnx.a` → **2026-02-04**，而主机是 2024-06 的 18.1.0。libnx master 面向新固件（新 IPC/新内核特性），在旧固件上的 sysmodule 会静默死掉（loader 拒绝或启动即崩）。

指纹参考：同一份源码
- libnx 2026 master 编译出 `swlink.nso` ≈ **38277** 字节
- libnx v4.12.0 编译出 `swlink.nso` ≈ **42487** 字节

## 4. 修复步骤（可复现）

```bash
# 1) 备份新 libnx，避免破坏
mv /opt/devkitpro/libnx /opt/devkitpro/libnx.2026

# 2) 取与固件同期的 libnx（18.1.0 对应 v4.12.0）
curl -sSL -o /tmp/libnx412.tar.gz \
  https://api.github.com/repos/switchbrew/libnx/tarball/refs/tags/v4.12.0
mkdir -p /tmp/libnx412 && tar xzf /tmp/libnx412.tar.gz -C /tmp/libnx412 --strip-components=1

# 3) 编译安装（关键：PATH 里要有 devkitA64 的 bin）
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitA64
export PATH=/tmp/bin:/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:$PATH
cd /tmp/libnx412 && make -j8 && make install

# 4) 重新编译 sysmodule
cd <project>/switch && make clean && make -j4
```

### 坑：`bin2s: command not found`

libnx 编译时要用 `bin2s`（打包 `nx/data/default_font.bin`），它属于 devkitPro 的
`general-tools` 包，而本机只装了 `switch-dev`，没有它。不用装包，写个替代脚本即可：

```python
#!/usr/bin/env python3
# /tmp/bin/bin2s —— 生成 .incbin 汇编 + 头文件
import sys, os
align, header, files = 4, None, []
i = 1
while i < len(sys.argv):
    a = sys.argv[i]
    if a == '-a': align = int(sys.argv[i+1]); i += 2
    elif a == '-H': header = sys.argv[i+1]; i += 2
    elif a.startswith('-a'): align = int(a[2:]); i += 1
    elif a.startswith('-H'): header = a[2:]; i += 1
    elif a.startswith('-'): i += 1
    else: files.append(a); i += 1
path = os.path.abspath(files[0])
name = os.path.basename(path).replace('.', '_')
sys.stdout.write('\n'.join([
    '.section .rodata', '.align %d' % align,
    '.global %s' % name, '%s:' % name, '.incbin "%s"' % path,
    '.global %s_end' % name, '%s_end:' % name,
    '.section .rodata', '.align 2',
    '.global %s_size' % name, '%s_size:' % name,
    '.int %s_end - %s' % (name, name),
]) + '\n')
if header:
    open(header, 'w').write(
        '#pragma once\n#include <stdint.h>\n#ifdef __cplusplus\nextern "C" {\n#endif\n'
        'extern const uint8_t %s[];\nextern const uint8_t %s_end[];\nextern const uint32_t %s_size;\n'
        '#ifdef __cplusplus\n}\n#endif\n' % (name, name, name))
```

### 顺带做的两处加固

- `main_thread_stack_size` 4KB → **32KB**：libnx 新版启动代码栈占用更大，4KB 有溢出风险（官方 SysDVR 4KB 是够的，但我们的 `main` 更重）。
- 把"禁用堆 + `fatalThrow`"的覆盖改成 **256KB 静态内堆**：任何一次 `aligned_alloc` 都会直接 fatal，是典型的"无日志静默死亡"来源；静态堆让这种情况至少能继续跑并留下日志。
- 在 `__appInit` 里 fs 挂载完成后立刻写一行日志，把崩溃点往前推。

## 5. 部署与验证

```bash
cp swlink.nsp /Volumes/<SD>/atmosphere/contents/00FF0000A53B5A57/exefs.nsp
touch /Volumes/<SD>/atmosphere/contents/00FF0000A53B5A57/flags/boot2.flag
```

开机后验证三件事：Tesla 里 SwLink 变成"已启用"、SD 根出现 `/SwLink.log`、Mac 上能枚举到 USB 设备。

## 6. 附录：本例顺带发现的 SysDVR / Tesla 配置坑

**SysDVR 不出 USB 设备**：它的模式由文件存在性决定（源码 `sysmodule/source/sysmodule/main.c`）：

```c
if (FileExists("/config/sysdvr/usb"))       SetModeID(TYPE_MODE_USB);
else if (FileExists("/config/sysdvr/rtsp")) SetModeID(TYPE_MODE_RTSP);
else if (FileExists("/config/sysdvr/tcp"))  SetModeID(TYPE_MODE_TCP);
```

而发行包自带的配置是 `config/sysdvr/rtsp` → 默认走 **RTSP 网络模式**，所以永远不出 USB 设备。
改成 USB 模式：`touch config/sysdvr/usb && rm config/sysdvr/rtsp`（只在开机时读取，需重启）。
USB 设备 ID 为 `18D1:4EE0`（Google Nexus 通用 ID，产品名 "SysDVR"）。

**Tesla 按不出来**：`config/tesla/config.ini` 里的 `key_combo` 被 Ultrahand 改成了 `ZL+ZR+DDOWN`，
不是常见的 `L+↓+R3`。已改为 `L+DDOWN+R`（同时按住 L 和 R，再按一下十字↓）。

## 7. 通用结论

对旧固件主机做 sysmodule 开发时，**libnx 必须是与该固件同期或更早的版本**。
遇到"模块完全没反应、无日志、无崩溃报告、同卡上旧模块却正常"时，第一件事是：
`ls -la $(DEVKITPRO)/libnx/lib/libnx.a` 看构建日期，以及拿一个已知能跑的同类型 sysmodule 做交叉对照。
