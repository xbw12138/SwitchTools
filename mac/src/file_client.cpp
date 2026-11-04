/*
 * file_client.cpp
 */
#include "file_client.hpp"
#include "link.hpp"
#include "../../protocol/swlink_protocol.h"

#include <cstring>
#include <cstdio>

#define USB_TIMEOUT_MS 10000

std::vector<uint8_t> FileClient::packPath(const std::string& path)
{
    std::vector<uint8_t> out(2 + path.size());
    uint16_t len = (uint16_t)path.size();
    memcpy(out.data(), &len, 2);
    memcpy(out.data() + 2, path.data(), path.size());
    return out;
}

bool FileClient::request(uint32_t cmd, uint64_t offset, uint64_t size,
                         const std::vector<uint8_t>& payload,
                         std::vector<uint8_t>& respPayload,
                         uint64_t* respOffset, uint64_t* respSize)
{
    std::lock_guard<std::mutex> lock(ioMutex_);

    SwlinkFtHeader hdr = {};
    hdr.magic = SWLINK_FT_MAGIC;
    hdr.cmd = cmd;
    hdr.seq = ++seq_;
    hdr.offset = offset;
    hdr.size = size;
    hdr.payload_len = (uint32_t)payload.size();

    if (!link_.fileWrite(&hdr, sizeof(hdr), USB_TIMEOUT_MS)) {
        err_ = "发送请求头失败";
        return false;
    }
    if (hdr.payload_len && !link_.fileWrite(payload.data(), hdr.payload_len, USB_TIMEOUT_MS)) {
        err_ = "发送请求负载失败";
        return false;
    }

    SwlinkFtHeader resp = {};
    /* 设备把"响应头 + 负载"打包成一次 USB 写;必须用大缓冲一次读回,
       拆成 40 字节小读会把同一个包剩下的数据丢掉(表现为列表空/下载中断) */
    std::vector<uint8_t> buf(sizeof(SwlinkFtHeader) + SWLINK_FT_CHUNK);
    size_t filled = 0;
    int timeouts = 0;

    while (filled < sizeof(SwlinkFtHeader)) {
        int got = link_.fileReadSome(buf.data() + filled, (int)(buf.size() - filled), USB_TIMEOUT_MS);
        if (got < 0) {
            err_ = "接收响应时连接断开";
            return false;
        }
        if (got == 0) {
            if (++timeouts > 2) {
                err_ = "接收响应头超时";
                return false;
            }
            continue;
        }
        filled += (size_t)got;
    }
    timeouts = 0;

    memcpy(&resp, buf.data(), sizeof(resp));
    if (resp.magic != SWLINK_FT_MAGIC) {
        err_ = "响应协议错位";
        return false;
    }
    if (resp.payload_len > SWLINK_FT_CHUNK) {
        err_ = "响应数据过大";
        return false;
    }

    const size_t need = sizeof(SwlinkFtHeader) + resp.payload_len;
    while (filled < need) {
        int got = link_.fileReadSome(buf.data() + filled, (int)(buf.size() - filled), USB_TIMEOUT_MS);
        if (got < 0 || (got == 0 && ++timeouts > 2)) {
            err_ = "接收响应数据失败";
            return false;
        }
        if (got > 0)
            filled += (size_t)got;
    }

    if (resp.status != SWLINK_FT_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "设备错误 %u", resp.status);
        err_ = msg;
        return false;
    }

    respPayload.assign(buf.begin() + sizeof(SwlinkFtHeader),
                       buf.begin() + (ptrdiff_t)need);

    if (respOffset) *respOffset = resp.offset;
    if (respSize) *respSize = resp.size;
    return true;
}

bool FileClient::ping()
{
    std::vector<uint8_t> out;
    return request(SwlinkFt_Ping, 0, 0, {}, out);
}

bool FileClient::list(const std::string& path, std::vector<FtEntry>& out)
{
    out.clear();
    uint64_t startIndex = 0;

    for (;;) {
        if (cancelled()) { err_ = "已取消"; return false; }
        std::vector<uint8_t> payload;
        uint64_t next = 0;
        if (!request(SwlinkFt_List, startIndex, 0, packPath(path), payload, &next))
            return false;

        const uint8_t* p = payload.data();
        const uint8_t* end = p + payload.size();
        while (p + 11 <= end) {
            FtEntry e;
            e.type = p[0];
            memcpy(&e.size, p + 1, 8);
            uint16_t nl;
            memcpy(&nl, p + 9, 2);
            p += 11;
            if (p + nl > end) break;
            e.name.assign((const char*)p, nl);
            p += nl;
            out.push_back(std::move(e));
        }

        if (next == 0) break;   /* 读完 */
        startIndex = next;
    }
    return true;
}

bool FileClient::stat(const std::string& path, uint8_t& type, uint64_t& size)
{
    std::vector<uint8_t> payload;
    if (!request(SwlinkFt_Stat, 0, 0, packPath(path), payload))
        return false;
    if (payload.size() < 9) { err_ = "响应过短"; return false; }
    type = payload[0];
    memcpy(&size, payload.data() + 1, 8);
    return true;
}

bool FileClient::statFs(const std::string& path, uint64_t& total, uint64_t& free)
{
    std::vector<uint8_t> payload;
    if (!request(SwlinkFt_StatFs, 0, 0, packPath(path), payload))
        return false;
    if (payload.size() < 16) { err_ = "响应过短"; return false; }
    memcpy(&total, payload.data(), 8);
    memcpy(&free, payload.data() + 8, 8);
    return true;
}

bool FileClient::readFile(const std::string& remote, const std::string& local, ProgressCb cb)
{
    uint8_t type;
    uint64_t total = 0;
    if (!stat(remote, type, total)) return false;
    if (type != 2) { err_ = "不是文件"; return false; }

    FILE* f = fopen(local.c_str(), "wb");
    if (!f) { err_ = "无法创建本地文件"; return false; }

    uint64_t done = 0;
    bool ok = true;
    while (done < total) {
        if (cancelled()) {
            err_ = "已取消";
            ok = false;
            break;
        }
        uint64_t want = total - done;
        if (want > SWLINK_FT_CHUNK) want = SWLINK_FT_CHUNK;

        std::vector<uint8_t> payload;
        uint64_t got = 0;
        if (!request(SwlinkFt_Read, done, want, packPath(remote), payload, nullptr, &got)) {
            ok = false;
            break;
        }
        if (got == 0) break;    /* 异常提前结束 */

        fwrite(payload.data(), 1, got, f);
        done += got;
        if (cb) cb(done, total);
    }

    fclose(f);
    if (!ok) std::remove(local.c_str());
    return ok;
}

bool FileClient::writeFile(const std::string& local, const std::string& remote, ProgressCb cb)
{
    FILE* f = fopen(local.c_str(), "rb");
    if (!f) { err_ = "无法打开本地文件"; return false; }

    fseek(f, 0, SEEK_END);
    uint64_t total = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint64_t done = 0;
    bool ok = true;

    /* 请求负载 = 路径 + 数据,设备端限制整个 payload 不超过 SWLINK_FT_CHUNK */
    const std::vector<uint8_t> pathPayload = packPath(remote);
    if (pathPayload.size() >= SWLINK_FT_CHUNK) {
        err_ = "目标路径过长";
        fclose(f);
        return false;
    }
    const uint64_t chunkMax = SWLINK_FT_CHUNK - pathPayload.size();

    while (done < total) {
        if (cancelled()) {
            err_ = "已取消";
            ok = false;
            break;
        }
        uint64_t want = total - done;
        if (want > chunkMax) want = chunkMax;

        /* payload = [u16 len][path][data...] */
        std::vector<uint8_t> payload = pathPayload;
        size_t pathBytes = payload.size();
        payload.resize(pathBytes + want);
        if (fread(payload.data() + pathBytes, 1, want, f) != want) {
            err_ = "读取本地文件失败";
            ok = false;
            break;
        }

        std::vector<uint8_t> resp;
        if (!request(SwlinkFt_Write, done, want, payload, resp)) {
            ok = false;
            break;
        }

        done += want;
        if (cb) cb(done, total);
    }

    fclose(f);
    return ok;
}

bool FileClient::mkdir(const std::string& path)
{
    std::vector<uint8_t> out;
    return request(SwlinkFt_Mkdir, 0, 0, packPath(path), out);
}

bool FileClient::remove(const std::string& path, bool isDir)
{
    std::vector<uint8_t> out;
    return request(SwlinkFt_Delete, isDir ? 1 : 0, 0, packPath(path), out);
}

bool FileClient::rename(const std::string& oldPath, const std::string& newPath)
{
    std::vector<uint8_t> payload = packPath(oldPath);
    std::vector<uint8_t> np = packPath(newPath);
    payload.insert(payload.end(), np.begin(), np.end());
    std::vector<uint8_t> out;
    return request(SwlinkFt_Rename, 0, 0, payload, out);
}
