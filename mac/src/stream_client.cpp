/*
 * stream_client.cpp
 */
#include "stream_client.hpp"
#include "usb_link.hpp"
#include "../../protocol/swlink_protocol.h"

#include <vector>
#include <cstring>

void StreamClient::start(VideoCb v, AudioCb a, DropCb d)
{
    if (running_) return;
    running_ = true;
    th_ = std::thread(&StreamClient::loop, this, std::move(v), std::move(a), std::move(d));
}

void StreamClient::stop()
{
    running_ = false;
    if (th_.joinable()) th_.join();
}

void StreamClient::loop(VideoCb vcb, AudioCb acb, DropCb dcb)
{
    std::vector<uint8_t> payload(SWLINK_VIDEO_BUF_SIZE + 64);

    while (running_) {
        SwlinkStreamHeader hdr;
        /* 短超时便于响应 stop;超时重试 */
        if (!link_.streamRead(&hdr, sizeof(hdr), 300)) {
            if (!running_) break;
            continue;
        }

        if (hdr.magic != SWLINK_PKT_MAGIC || hdr.data_size > payload.size()) {
            /* 协议错位,视为断链 */
            break;
        }

        if (hdr.data_size > 0) {
            if (!link_.streamRead(payload.data(), hdr.data_size, 1000))
                break;
        }

        if (hdr.flags & SWLINK_STREAM_FLAG_ERROR)
            continue;   /* 设备侧 grc 错误包,直接丢弃 */

        if (hdr.type == SwlinkStream_Video)
            vcb(payload.data(), hdr.data_size, hdr.flags);
        else if (hdr.type == SwlinkStream_Audio)
            acb(payload.data(), hdr.data_size);
    }

    running_ = false;
    if (dcb) dcb();
}
