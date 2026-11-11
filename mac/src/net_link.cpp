/*
 * net_link.cpp
 * 局域网 TCP 传输层实现
 */
#include "net_link.hpp"
#include "../../protocol/swlink_protocol.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

static int remainingMs(std::chrono::steady_clock::time_point deadline)
{
    auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    if (left < 0) return 0;
    if (left > 60000) return 60000;
    return (int)left;
}

NetLink::~NetLink()
{
    close();
}

bool NetLink::open()
{
    close();

    if (ip_.empty()) {
        snprintf(err_, sizeof(err_), "请先填写 Switch 的 IP 地址");
        return false;
    }

    fd_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        snprintf(err_, sizeof(err_), "创建 socket 失败: %s", strerror(errno));
        return false;
    }

    int one = 1;
#ifdef SO_NOSIGPIPE
    setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    if (inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr) != 1) {
        snprintf(err_, sizeof(err_), "IP 地址格式无效: %s", ip_.c_str());
        close();
        return false;
    }

    /* 非阻塞 connect + poll,避免不可达地址卡住 UI */
    const int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd_, (sockaddr*)&addr, sizeof(addr));
    if (rc != 0 && errno != EINPROGRESS) {
        snprintf(err_, sizeof(err_), "连接 %s:%u 失败: %s",
                 ip_.c_str(), (unsigned)port_, strerror(errno));
        close();
        return false;
    }

    if (rc != 0) {
        pollfd p = { fd_, POLLOUT, 0 };
        if (poll(&p, 1, 3000) <= 0) {
            snprintf(err_, sizeof(err_),
                     "连接 %s:%u 超时。请确认 Switch 与 Mac 在同一个 Wi-Fi,"
                     "且 sysmodule 已启动。", ip_.c_str(), (unsigned)port_);
            close();
            return false;
        }
        int soErr = 0;
        socklen_t sl = sizeof(soErr);
        getsockopt(fd_, SOL_SOCKET, SO_ERROR, &soErr, &sl);
        if (soErr != 0) {
            snprintf(err_, sizeof(err_), "连接 %s:%u 失败: %s",
                     ip_.c_str(), (unsigned)port_, strerror(soErr));
            close();
            return false;
        }
    }

    fcntl(fd_, F_SETFL, flags);
    return true;
}

void NetLink::close()
{
    stopHeartbeat();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void NetLink::stopHeartbeat()
{
    heartbeatRun_ = false;
    if (heartbeat_.joinable())
        heartbeat_.join();
}

void NetLink::heartbeatLoop()
{
    while (heartbeatRun_) {
        for (int i = 0; i < 20 && heartbeatRun_; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!heartbeatRun_)
            break;

        const int fd = fd_;
        if (fd < 0)
            break;

        uint8_t ping = SWLINK_NET_PING;
        ssize_t n = ::send(fd, &ping, 1, MSG_DONTWAIT);
        if (n == 1)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            continue;
        /* 发送失败:交给读线程判定断开,这里直接退出 */
        break;
    }
}

bool NetLink::sendAll(const void* data, int len, int timeoutMs)
{
    const uint8_t* p = (const uint8_t*)data;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (len > 0) {
        ssize_t n = ::send(fd_, p, (size_t)len, 0);
        if (n > 0) {
            p += n;
            len -= (int)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            int left = remainingMs(deadline);
            if (left <= 0) return false;
            pollfd po = { fd_, POLLOUT, 0 };
            poll(&po, 1, left);
            continue;
        }
        return false;
    }
    return true;
}

bool NetLink::recvExact(void* data, int len, int timeoutMs)
{
    uint8_t* p = (uint8_t*)data;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    while (len > 0) {
        ssize_t n = ::recv(fd_, p, (size_t)len, 0);
        if (n > 0) {
            p += n;
            len -= (int)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            int left = remainingMs(deadline);
            if (left <= 0) return false;
            pollfd po = { fd_, POLLIN, 0 };
            poll(&po, 1, left);
            continue;
        }
        return false;
    }
    return true;
}

bool NetLink::handshake(bool wantVideo, bool wantAudio, uint8_t audioBatching, int timeoutMs)
{
    /* 设备连上后先发 10 字节 HELLO;滑窗扫描以容忍字节流错位 */
    uint8_t win[64];
    int filled = 0;
    bool found = false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!found) {
        int left = remainingMs(deadline);
        if (left <= 0) break;

        pollfd p = { fd_, POLLIN, 0 };
        if (poll(&p, 1, left > 200 ? 200 : left) <= 0)
            continue;

        uint8_t tmp[32];
        ssize_t got = ::recv(fd_, tmp, sizeof(tmp), 0);
        if (got <= 0) {
            snprintf(err_, sizeof(err_), "等待 HELLO 时连接断开");
            return false;
        }

        if (filled + (int)got > (int)sizeof(win)) {
            memmove(win, win + filled - 16, 16);
            filled = 16;
        }
        memcpy(win + filled, tmp, (size_t)got);
        filled += (int)got;

        for (int i = 0; i + 10 <= filled; i++) {
            if (memcmp(win + i, SWLINK_HELLO, 10) == 0) {
                found = true;
                break;
            }
        }
    }

    if (!found) {
        snprintf(err_, sizeof(err_), "未收到 HELLO(设备未启动无线服务?)");
        return false;
    }

    SwlinkHandshakeRequest req = {};
    req.magic = SWLINK_HANDSHAKE_MAGIC;
    req.proto_ver = ('1' << 8) | '0';
    req.want_video = wantVideo ? 1 : 0;
    req.want_audio = wantAudio ? 1 : 0;
    req.audio_batching = audioBatching;

    if (!sendAll(&req, sizeof(req), timeoutMs)) {
        snprintf(err_, sizeof(err_), "发送握手请求失败");
        return false;
    }

    SwlinkHandshakeResponse resp = {};
    if (!recvExact(&resp, sizeof(resp), timeoutMs)) {
        snprintf(err_, sizeof(err_), "接收握手响应失败");
        return false;
    }
    if (resp.code != SWLINK_HANDSHAKE_OK) {
        snprintf(err_, sizeof(err_), "握手被拒, code=%u", resp.code);
        return false;
    }

    /* 会话建立后开始心跳,设备端 10s 收不到任何活动会主动断开 */
    stopHeartbeat();
    heartbeatRun_ = true;
    heartbeat_ = std::thread(&NetLink::heartbeatLoop, this);
    return true;
}

int NetLink::streamReadSome(void* data, int maxLen, unsigned timeoutMs)
{
    pollfd p = { fd_, POLLIN, 0 };
    int pr = poll(&p, 1, (int)timeoutMs);
    if (pr == 0) return 0;
    if (pr < 0) {
        if (errno == EINTR) return 0;
        return -1;
    }

    ssize_t n = ::recv(fd_, data, (size_t)maxLen, 0);
    if (n > 0)
        return (int)n;
    if (n == 0) return -1;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

int NetLink::fileReadSome(void* data, int maxLen, unsigned timeoutMs)
{
    (void)data; (void)maxLen; (void)timeoutMs;
    snprintf(err_, sizeof(err_), "Wi-Fi 模式暂不支持 SD 卡文件传输(请用 USB)");
    return -1;
}

bool NetLink::fileWrite(const void* data, int len, unsigned timeoutMs)
{
    (void)data; (void)len; (void)timeoutMs;
    snprintf(err_, sizeof(err_), "Wi-Fi 模式暂不支持 SD 卡文件传输(请用 USB)");
    return false;
}

std::vector<NetDevice> netDiscover(int timeoutMs)
{
    std::vector<NetDevice> out;

    int fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return out;

    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif

    SwlinkDiscoveryMsg req = {};
    memcpy(req.magic, SWLINK_DISCOVER_REQ, sizeof(SWLINK_DISCOVER_REQ));
    req.proto_ver = ('1' << 8) | '0';

    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(SWLINK_DISCOVER_PORT);
    to.sin_addr.s_addr = INADDR_BROADCAST;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto nextSend = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() < deadline) {
        auto now = std::chrono::steady_clock::now();
        if (now >= nextSend) {
            sendto(fd, &req, sizeof(req), 0, (sockaddr*)&to, sizeof(to));
            nextSend = now + std::chrono::milliseconds(400);
        }

        pollfd p = { fd, POLLIN, 0 };
        if (poll(&p, 1, 100) <= 0)
            continue;

        SwlinkDiscoveryMsg resp = {};
        sockaddr_in from = {};
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(fd, &resp, sizeof(resp), 0, (sockaddr*)&from, &flen);
        if (n != (ssize_t)sizeof(resp))
            continue;
        if (strncmp(resp.magic, SWLINK_DISCOVER_RESP, sizeof(resp.magic)) != 0)
            continue;

        char ip[INET_ADDRSTRLEN] = {0};
        if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)))
            continue;

        bool dup = false;
        for (const auto& d : out) {
            if (d.ip == ip) { dup = true; break; }
        }
        if (dup)
            continue;

        NetDevice dev;
        dev.ip = ip;
        dev.port = resp.tcp_port ? resp.tcp_port : SWLINK_TCP_PORT;
        out.push_back(dev);
    }

    ::close(fd);
    return out;
}
