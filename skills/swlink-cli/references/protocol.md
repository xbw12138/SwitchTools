# SwLink USB 文件传输协议

与串流接口共存于同一个 USB 复合设备（VID `0x16c0`、PID `0x27da`），三个接口：
Interrupt（握手/输入）、Stream（视频/音频）、File（文件传输）。

## 请求头

`SwlinkFtHeader`，40 字节、packed，定义在 `protocol/swlink_protocol.h`。

| 字段 | 类型 | 说明 |
|---|---|---|
| `magic` | u32 | `SWLINK_FT_MAGIC` |
| `cmd` | u32 | 命令号 |
| `status` | u32 | 响应状态 |
| `seq` | u32 | 序号，响应回显 |
| `offset` | u64 | 传输偏移 / LIST 起始索引 |
| `size` | u64 | 长度 / 返回字节数或条目数 |
| `payload_len` | u32 | 负载长度 |
| `reserved` | — | 对齐填充至 40 字节 |

## 命令

| cmd | 名称 | 请求负载 | 响应 |
|---|---|---|---|
| Ping | `SwlinkFt_Ping` | 无 | 无 |
| List | `SwlinkFt_List` | 路径 | 条目数组 |
| Stat | `SwlinkFt_Stat` | 路径 | `u8 type` + `u64 size`（9 字节紧凑布局） |
| Read | `SwlinkFt_Read` | 路径 | 文件数据 |
| Write | `SwlinkFt_Write` | 路径 + 数据 | 无 |
| StatFs | `SwlinkFt_StatFs` | 路径 | `u64 total` + `u64 free` |
| Mkdir | `SwlinkFt_Mkdir` | 路径 | 无 |
| Delete | `SwlinkFt_Delete` | 路径（`offset` 1 = 目录递归） | 无 |
| Rename | `SwlinkFt_Rename` | 旧路径 + 新路径 | 无 |

状态码：`1` OK、`2` 路径不存在、`3` IO 错误、`4` 参数错误、`5` 路径过长。

## 负载格式

路径统一编码为 `[u16 len][bytes]`，必须 `/` 开头，限制在 SD 根内。

- List：每条目 `u8 type`（1 目录 / 2 文件）+ `u64 size` + `u16 name_len` + 名字；单批最多 `SWLINK_FT_LIST_BATCH` 条，`resp.offset` 返回下一批起始索引，`0` 表示读完
- Write：`[u16 len][path][data...]`，整包不超过 `SWLINK_FT_CHUNK`
- Rename：两个路径编码依次拼接

## 关键约束

1. **单块上限** `SWLINK_FT_CHUNK`（约 512KB）。Write 的负载含路径，客户端必须按路径长度扣减可写数据量
2. **单次 USB 传输切片** `FT_IO_SLICE`（32KB）。主机侧同样按 32KB 切片，两侧对齐
3. **4KB 对齐**：设备端收发缓冲必须 4KB 对齐。非对齐缓冲会让 `usb_link` 退化为 4KB 中转，主机一次写 32KB 时超出部分被端点丢弃，该端点之后永久无法完成读请求（只能重启主机）
4. **Stat 不能使用结构体布局**：`struct { u8; u64; }` 会因对齐在中间插入 7 字节填充，`size` 落在偏移 8；必须用紧凑 9 字节
5. **响应一次写完**：设备把"头 + 负载"打包成一次 USB 写，客户端必须用大缓冲一次读回，拆成 40 字节小读会丢弃同一包剩余数据
6. **不做串流握手**：文件接口由设备端独立线程服务，CLI 不握手。握手会把设备置为"已连接"，客户端退出后设备无从感知，下次可能收不到 HELLO

## 设备端文件操作

全部走原生 fs IPC（`fsFsOpenFile` / `fsFileRead` / `fsFsCreateFile` / `fsFileSetSize`），不经过 fsdev：

- Write：`open(Write|Append)` → 失败则 `create(size=0)` + `open(Write)` → `SetSize(offset+len)` → `Write` → `Flush` → `Close`
- 先建 0 字节文件再扩展，避免请求预分配
- 每块独立 open/close，崩溃后不会留下半截句柄

## 相关源码

- `switch/source/file_server.c`：设备端服务
- `mac/src/file_client.cpp`：客户端请求封装
- `mac/src/usb_link.cpp`：libusb 传输层
- `protocol/swlink_protocol.h`：协议结构定义
