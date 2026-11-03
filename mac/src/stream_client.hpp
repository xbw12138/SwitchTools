/*
 * stream_client.hpp
 * 串流接收线程:按 18B 头 + payload 拆包,分发视频/音频
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

class Link;

class StreamClient {
public:
    using VideoCb = std::function<void(const uint8_t* data, uint32_t len, uint8_t flags)>;
    using AudioCb = std::function<void(const uint8_t* data, uint32_t len)>;
    using DropCb  = std::function<void()>;   /* 连接中断 */

    explicit StreamClient(Link& link) : link_(link) {}

    void start(VideoCb v, AudioCb a, DropCb d);
    void stop();
    bool running() const { return running_; }

private:
    void loop(VideoCb v, AudioCb a, DropCb d);

    Link& link_;
    std::thread th_;
    std::atomic<bool> running_{false};
};
