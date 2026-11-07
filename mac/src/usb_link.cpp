/*
 * usb_link.cpp
 */
#include "usb_link.hpp"
#include "../../protocol/swlink_protocol.h"

#include <cstring>
#include <cstdio>
#include <thread>
#include <chrono>

static void setErr(char* dst, size_t n, const char* msg, int rc)
{
    snprintf(dst, n, "%s: %s", msg, libusb_strerror((libusb_error)rc));
}

UsbLink::~UsbLink()
{
    close();
}

bool UsbLink::open()
{
    int rc = libusb_init(&ctx_);
    if (rc != 0) { setErr(err_, sizeof(err_), "libusb_init", rc); return false; }

    dev_ = libusb_open_device_with_vid_pid(ctx_, SWLINK_USB_VID, SWLINK_USB_PID);
    if (!dev_) {
        snprintf(err_, sizeof(err_),
                 "未找到 Switch 设备 %04X:%04X\n"
                 "  1) 确认用的是数据线(不是充电线),且 Switch 已开机\n"
                 "  2) 确认已安装 SwLink sysmodule:SD:/atmosphere/contents/00FF0000A53B5A57/"
                 " 下需有 exefs.nsp、toolbox.json、flags/boot2.flag\n"
                 "  3) 装好后重启主机,或在 Tesla 菜单的 Sysmodules 里启用 SwLink\n"
                 "  安装包: https://raw.githubusercontent.com/xbw12138/SwitchTools/main/dist/"
                 "swlink-sysmodule-sd.zip (解压到 SD 卡根目录即可)\n"
                 "  4) 若 SwLink.app 正在运行,先退出它(USB 接口独占)",
                 SWLINK_USB_VID, SWLINK_USB_PID);
        close();
        return false;
    }

    /* 解析配置描述符,按接口号找批量端点 */
    libusb_device* dev = libusb_get_device(dev_);
    libusb_config_descriptor* cfg = nullptr;
    rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc != 0) { setErr(err_, sizeof(err_), "get_active_config", rc); close(); return false; }

    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const libusb_interface& intf = cfg->interface[i];
        if (intf.num_altsetting < 1) continue;
        const libusb_interface_descriptor& d = intf.altsetting[0];
        for (int e = 0; e < d.bNumEndpoints; e++) {
            const libusb_endpoint_descriptor& ep = d.endpoint[e];
            if ((ep.bmAttributes & 0x3) != LIBUSB_TRANSFER_TYPE_BULK) continue;
            bool in = (ep.bEndpointAddress & 0x80) != 0;
            if (d.bInterfaceNumber == 0)
                (in ? epStreamIn_ : epStreamOut_) = ep.bEndpointAddress;
            else if (d.bInterfaceNumber == 1)
                (in ? epFileIn_ : epFileOut_) = ep.bEndpointAddress;
        }
    }
    libusb_free_config_descriptor(cfg);

    if (!epStreamIn_ || !epStreamOut_ || !epFileIn_ || !epFileOut_) {
        snprintf(err_, sizeof(err_), "端点不全 (设备固件异常)");
        close();
        return false;
    }

#ifdef __APPLE__
    /* macOS 独占:内核驱动占用时需要 detach */
    for (int intf = 0; intf < 2; intf++) {
        if (libusb_kernel_driver_active(dev_, intf) == 1)
            libusb_detach_kernel_driver(dev_, intf);
    }
#endif

    for (int intf = 0; intf < 2; intf++) {
        rc = libusb_claim_interface(dev_, intf);
        if (rc != 0) { setErr(err_, sizeof(err_), "claim_interface", rc); close(); return false; }
    }

    return true;
}

void UsbLink::close()
{
    if (dev_) {
        libusb_release_interface(dev_, 0);
        libusb_release_interface(dev_, 1);
        libusb_close(dev_);
        dev_ = nullptr;
    }
    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

/*
 * 单次 bulk 传输上限必须与设备端吻合(设备按 32KB 一档投递读写缓冲)。
 * 一次提交过大的传输,设备端偏移对不上就可能中途取消,而取消会在设备端
 * 留下一个永远不完成的挂起读请求,把文件端点彻底堵死,只能重启主机。
 */
static const int kIoSlice = 0x8000;

bool UsbLink::readExact(uint8_t ep, void* data, int len, unsigned timeoutMs)
{
    uint8_t* p = (uint8_t*)data;
    while (len > 0) {
        int want = len > kIoSlice ? kIoSlice : len;
        int got = 0;
        int rc = libusb_bulk_transfer(dev_, ep, p, want, &got, timeoutMs);
        if (rc == LIBUSB_ERROR_TIMEOUT && got == 0) return false;
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) return false;
        p += got;
        len -= got;
    }
    return true;
}

bool UsbLink::writeExact(uint8_t ep, const void* data, int len, unsigned timeoutMs)
{
    const uint8_t* p = (const uint8_t*)data;
    while (len > 0) {
        int want = len > kIoSlice ? kIoSlice : len;
        int sent = 0;
        int rc = libusb_bulk_transfer(dev_, ep, (uint8_t*)p, want, &sent, timeoutMs);
        if (rc == LIBUSB_ERROR_TIMEOUT && sent == 0) return false;
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) return false;
        p += sent;
        len -= sent;
    }
    return true;
}

bool UsbLink::streamRead(void* data, int len, unsigned timeoutMs)
{
    return readExact(epStreamIn_, data, len, timeoutMs);
}

int UsbLink::streamReadSome(void* data, int maxLen, unsigned timeoutMs)
{
    int got = 0;
    int rc = libusb_bulk_transfer(dev_, epStreamIn_, (uint8_t*)data, maxLen, &got, timeoutMs);
    if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT)
        return -1;
    return got;
}

bool UsbLink::fileRead(void* data, int len, unsigned timeoutMs)
{
    return readExact(epFileIn_, data, len, timeoutMs);
}

int UsbLink::fileReadSome(void* data, int maxLen, unsigned timeoutMs)
{
    int got = 0;
    int rc = libusb_bulk_transfer(dev_, epFileIn_, (uint8_t*)data, maxLen, &got, timeoutMs);
    if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT)
        return -1;
    return got;
}

bool UsbLink::fileWrite(const void* data, int len, unsigned timeoutMs)
{
    return writeExact(epFileOut_, data, len, timeoutMs);
}

bool UsbLink::handshake(bool wantVideo, bool wantAudio, uint8_t audioBatching, int timeoutMs)
{
    /* 设备循环发 10 字节 HELLO;读入缓冲并扫描定位,容忍字节流错位 */
    uint8_t win[64];
    int filled = 0;
    bool found = false;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline && !found) {
        uint8_t tmp[32];
        int got = 0;
        int rc = libusb_bulk_transfer(dev_, epStreamIn_, tmp, sizeof(tmp), &got, 500);
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) return false;
        if (got <= 0) continue;

        if (filled + got > (int)sizeof(win)) { /* 滑窗保留尾部 */
            memmove(win, win + filled - 16, 16);
            filled = 16;
        }
        memcpy(win + filled, tmp, got);
        filled += got;

        for (int i = 0; i + 10 <= filled; i++) {
            if (memcmp(win + i, SWLINK_HELLO, 10) == 0) { found = true; break; }
        }
    }
    if (!found) {
        snprintf(err_, sizeof(err_), "未收到 HELLO");
        return false;
    }

    SwlinkHandshakeRequest req = {};
    req.magic = SWLINK_HANDSHAKE_MAGIC;
    req.proto_ver = ('1' << 8) | '0';
    req.want_video = wantVideo ? 1 : 0;
    req.want_audio = wantAudio ? 1 : 0;
    req.audio_batching = audioBatching;

    if (!writeExact(epStreamOut_, &req, sizeof(req), timeoutMs)) {
        snprintf(err_, sizeof(err_), "发送握手请求失败");
        return false;
    }

    SwlinkHandshakeResponse resp = {};
    if (!readExact(epStreamIn_, &resp, sizeof(resp), timeoutMs)) {
        snprintf(err_, sizeof(err_), "接收握手响应失败");
        return false;
    }
    if (resp.code != SWLINK_HANDSHAKE_OK) {
        snprintf(err_, sizeof(err_), "握手被拒, code=%u", resp.code);
        return false;
    }
    return true;
}
