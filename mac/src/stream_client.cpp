/*
 * stream_client.cpp
 */
#include "stream_client.hpp"
#include "usb_link.hpp"
#include "../../protocol/swlink_protocol.h"

#include <vector>
#include <cstring>
#include <cstddef>

/* 单次 USB 批量读的上限:必须能一次收下设备可能发出的最大包(视频满缓冲 + 包头) */
static const int kReadMax = SWLINK_VIDEO_BUF_SIZE + 64;

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
    std::vector<uint8_t> chunk(kReadMax);
    std::vector<uint8_t> buf;      /* 已收到未解析的字节 */
    size_t off = 0;                /* buf 中已解析到的位置 */

    buf.reserve(kReadMax * 2);

    while (running_) {
        /* 1) 先解析缓冲区里已有的完整包 */
        while (off + sizeof(SwlinkStreamHeader) <= buf.size()) {
            SwlinkStreamHeader hdr;
            memcpy(&hdr, buf.data() + off, sizeof(hdr));

            if (hdr.magic != SWLINK_PKT_MAGIC || hdr.data_size > SWLINK_VIDEO_BUF_SIZE) {
                /* 流错位:逐字节找下一个同步点 */
                off++;
                continue;
            }

            const size_t total = sizeof(hdr) + hdr.data_size;
            if (off + total > buf.size())
                break;              /* 数据还不够,下一轮继续读 */

            const uint8_t* payload = buf.data() + off + sizeof(hdr);

            /* 设备侧 grc 错误包直接丢弃 */
            if (!(hdr.flags & SWLINK_STREAM_FLAG_ERROR)) {
                if (hdr.type == SwlinkStream_Video)
                    vcb(payload, hdr.data_size, hdr.flags);
                else if (hdr.type == SwlinkStream_Audio)
                    acb(payload, hdr.data_size);
            }
            off += total;
        }

        /* 2) 丢弃已消费字节(每轮都清,避免缓冲无限增长) */
        if (off > 0) {
            buf.erase(buf.begin(), buf.begin() + (ptrdiff_t)off);
            off = 0;
        }

        /* 3) 再读一段进来 */
        int got = link_.streamReadSome(chunk.data(), kReadMax, 100);
        if (got < 0) break;
        if (got > 0)
            buf.insert(buf.end(), chunk.data(), chunk.data() + got);

        /* 保护:长时间只增不减说明握手/设备异常 */
        if (buf.size() > (size_t)kReadMax * 3) break;
    }

    running_ = false;
    if (dcb) dcb();
}
