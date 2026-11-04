# swlink-cli 排障手册

## 1. 找不到设备

现象：`连接 Switch 失败: 未找到 Switch 设备 16C0:27DA`

注意：`libusb_open_device_with_vid_pid` 在"设备不存在"和"设备存在但打不开"两种情况下都返回空，所以报错文案会一样。

按顺序排查：

```sh
# 1) 系统是否枚举到设备
system_profiler SPUSBDataType | grep -A6 SwLink
```

- 完全没有 `SwLink Switch`：换数据线、换机身口（不要走 hub / 扩展坞 / 显示器）、确认 Switch 亮屏未休眠、确认 sysmodule 已启动（开机后等 10 秒）
- 能看到 `Vendor ID: 0x16c0`、`Product ID: 0x27da` 但 CLI 仍连不上：接口被别的进程占用（退出 SwLink.app），或者上一次传输异常导致端点未释放——拔插 USB 线，必要时重启主机
- 曾经正常、突然消失：优先怀疑换了端口（Hub 会让设备完全枚举不上）

## 2. 权限/占用错误

现象：`claim_interface: Access denied`，或 `USBDeviceOpen` 失败码 `0xe00002d8`，或 `libusb_open` 返回 `-99`

原因：IOKit 独占。最常见是 SwLink.app 还在运行（它是 GUI，占着同一组接口）。

```sh
pgrep -fil swlink     # 应无输出
```

退出 App 后再试；若有残留进程，结束它。

## 3. 设备错误 3（IO）

现象：`上传失败 ...: 设备错误 3`，或 `读取 <path> 失败: 设备错误 3`

最常见原因是**远端父路径不存在**：`put` 把第二个参数当目录拼接时会产生 `/switch/a.txt/a.txt` 这类路径。

- 用完整目标路径重试：`put ./a.txt /switch/a.txt`
- 先 `ls` 确认父目录存在，不存在就先 `mkdir`
- 设备端日志会在建文件失败时打印 `FT create rc=... free=... type=... path=...`，可直接看真实路径与 errno

状态码 `2` 才是"路径不存在"；若拿到 `3` 但路径看起来正确，怀疑 SD 卡文件系统异常（见第 6 条）。

## 4. 下载得到 0 字节

现象：`get` 成功退出但本地文件为空，进度行不出现或 total 是 `72057594037927936` 这类错位值。

旧固件的 `Stat` 响应用了 `struct { u8 type; u64 size; }`，对齐填充让 `size` 落在偏移 8，客户端按偏移 1 解析会得到错位值；低字节为 0 的文件尺寸直接被读成 0，循环不执行。

处理：升级设备端 `exefs.nsp` 到使用 9 字节紧凑布局的版本。

## 5. 传输中途失败，之后连 `ls` 都不通

现象：大文件上传到一半失败，随后所有命令都超时。

原因：端点被打废。`usb_link` 对非 4KB 对齐缓冲会走 4KB 中转，主机一次写超量后端点无法完成读请求。

处理：拔插 USB 线（必要时重启 Switch）。固件侧需保证收发缓冲 4KB 对齐、单次传输按 32KB 切片。

## 6. SD 卡本身异常

现象：能 `ls`、能 `mkdir`，但建文件/写文件失败；或长时间使用后突然写入全挂。

在 Mac 上只读校验：

```sh
diskutil verifyVolume /Volumes/<卷名>
```

若报 `found corrupt and needs to be repaired`：先备份重要数据，再 `diskutil repairVolume`。exFAT 损坏修复可能丢失少量损坏目录项。

## 7. 大文件上传被拒

现象：`设备错误 4`（参数错误），或大文件写成半截。

`Write` 请求负载 = 路径 + 数据，整包必须 ≤ `SWLINK_FT_CHUNK`。自研客户端要按 `CHUNK - 路径字节数` 扣减单块数据量，最后一包不能超。

## 8. 进度与输出

- 进度条写 stderr，可与 `2>/dev/null` 分离
- `ls` 输出格式：`  [目录] name` 或 `  <12 位右对齐> size  name`，末尾附 `共 N 项`
- 退出码：`0` 成功、`1` 失败；脚本中据此判断
