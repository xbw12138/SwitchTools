/*
 * link.hpp
 * 传输链路抽象:USB 与局域网 TCP 共用同一份字节协议
 */
#pragma once

#include <cstdint>

class Link {
public:
    virtual ~Link() = default;

    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    /* 串流握手:等待 HELLO -> 发请求 -> 收响应 */
    virtual bool handshake(bool wantVideo, bool wantAudio, uint8_t audioBatching,
                           int timeoutMs = 5000) = 0;

    /* 单次批量读:返回字节数,0 = 超时,<0 = 断开 */
    virtual int streamReadSome(void* data, int maxLen, unsigned timeoutMs) = 0;
    virtual int fileReadSome(void* data, int maxLen, unsigned timeoutMs) = 0;
    virtual bool fileWrite(const void* data, int len, unsigned timeoutMs) = 0;

    virtual const char* lastError() const = 0;
};
