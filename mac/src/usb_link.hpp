/*
 * usb_link.hpp
 * libusb 设备连接层:发现设备、声明两个接口、端点读写
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <libusb.h>

#include "link.hpp"

class UsbLink : public Link {
public:
    UsbLink() = default;
    ~UsbLink() override;

    UsbLink(const UsbLink&) = delete;
    UsbLink& operator=(const UsbLink&) = delete;

    /* 按协议 VID/PID 查找设备并声明接口 0(流)/1(文件) */
    bool open() override;
    void close() override;
    bool isOpen() const override { return dev_ != nullptr; }

    /* 流接口握手:等待 HELLO -> 发请求 -> 收响应 */
    bool handshake(bool wantVideo, bool wantAudio, uint8_t audioBatching, int timeoutMs = 5000) override;

    /* 精确长度读写;false = 出错或断开 */
    bool streamRead(void* data, int len, unsigned timeoutMs);
    /* 单次批量读:返回实际收到的字节数,0 = 超时,<0 = 断开。
       必须用大缓冲一次读够,小缓冲会把一个 USB 包剩余数据丢掉导致流错位 */
    int streamReadSome(void* data, int maxLen, unsigned timeoutMs) override;
    bool fileRead(void* data, int len, unsigned timeoutMs);
    /* 单次批量读,返回实际字节数(0=超时,<0=断开);响应头和负载必须一次读完 */
    int fileReadSome(void* data, int maxLen, unsigned timeoutMs) override;
    bool fileWrite(const void* data, int len, unsigned timeoutMs) override;

    const char* lastError() const override { return err_; }

private:
    bool readExact(uint8_t ep, void* data, int len, unsigned timeoutMs);
    bool writeExact(uint8_t ep, const void* data, int len, unsigned timeoutMs);

    libusb_context* ctx_ = nullptr;
    libusb_device_handle* dev_ = nullptr;
    uint8_t epStreamIn_ = 0, epStreamOut_ = 0;
    uint8_t epFileIn_ = 0, epFileOut_ = 0;
    char err_[128] = {0};
};
