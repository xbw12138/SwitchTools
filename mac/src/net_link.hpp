/*
 * net_link.hpp
 * 局域网 TCP 传输层:连接设备串流端口,字节协议与 USB 完全一致
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "link.hpp"

struct NetDevice {
    std::string ip;
    uint16_t port = 0;
};

/* 向局域网广播发现请求,返回在 timeoutMs 内响应的设备 */
std::vector<NetDevice> netDiscover(int timeoutMs = 1200);

class NetLink : public Link {
public:
    NetLink() = default;
    ~NetLink() override;

    NetLink(const NetLink&) = delete;
    NetLink& operator=(const NetLink&) = delete;

    void setEndpoint(const std::string& ip, uint16_t port)
    {
        ip_ = ip;
        port_ = port;
    }

    const std::string& ip() const { return ip_; }
    uint16_t port() const { return port_; }

    bool open() override;
    void close() override;
    bool isOpen() const override { return fd_ >= 0; }

    bool handshake(bool wantVideo, bool wantAudio, uint8_t audioBatching,
                   int timeoutMs = 5000) override;

    int streamReadSome(void* data, int maxLen, unsigned timeoutMs) override;

    /* 无线模式暂不实现文件通道 */
    int fileReadSome(void* data, int maxLen, unsigned timeoutMs) override;
    bool fileWrite(const void* data, int len, unsigned timeoutMs) override;
    bool supportsFileTransfer() const override { return false; }

    const char* lastError() const override { return err_; }

private:
    bool sendAll(const void* data, int len, int timeoutMs);
    bool recvExact(void* data, int len, int timeoutMs);
    void heartbeatLoop();
    void stopHeartbeat();

    std::string ip_;
    uint16_t port_ = 0;
    int fd_ = -1;
    char err_[192] = {0};

    std::thread heartbeat_;
    std::atomic<bool> heartbeatRun_{false};
};
