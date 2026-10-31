/*
 * usb_comms.h
 * usb:ds 初始化与批量端点读写 (参考 SysDVR UsbComms,仅保留 5.0+ 路径,双接口)
 */
#pragma once
#include <switch.h>

/* 接口编号 */
enum {
    USB_INTF_STREAM = 0, /* 串流: EP OUT 握手, EP IN 数据 */
    USB_INTF_FILE   = 1, /* 文件: EP OUT 请求, EP IN 响应 */
    USB_NUM_INTERFACES
};

/* 初始化 usb:ds,注册两个 vendor-specific 接口并使能设备 */
Result swlinkUsbInitialize(void);

/* 关闭所有接口并退出 usb:ds */
void swlinkUsbExit(void);

/*
 * 从指定接口的 OUT 端点读取,返回实际读取字节数。
 * timeout 单位纳秒;超时返回已读到的字节数(可能为 0)。
 */
size_t swlinkUsbRead(u32 interface, void* buffer, size_t size, u64 timeout);

/* 向指定接口的 IN 端点写入,返回实际写入字节数 */
size_t swlinkUsbWrite(u32 interface, const void* buffer, size_t size, u64 timeout);
