/*
 * stream_server.c
 * 串流服务实现:HELLO 握手,视频/音频复用同一 IN 端点
 */
#include <string.h>
#include "stream_server.h"
#include "capture.h"
#include "usb_link.h"
#include "../../protocol/swlink_protocol.h"

#define HANDSHAKE_TIMEOUT 1E+9  /* 1s */
#define SEND_TIMEOUT      1E+9

static bool videoConnected = false;
static bool audioConnected = false;
static Mutex sendMutex;

static bool clientConnected(void)
{
    return videoConnected || audioConnected;
}

static void disconnectClient(void)
{
    videoConnected = false;
    audioConnected = false;
}

/* 视频/音频线程共用的发送入口,互斥保护 IN 端点 */
static bool sendPacket(const void* data, size_t length)
{
    bool success;

    mutexLock(&sendMutex);
    if (!clientConnected())
        success = false;
    else
        success = swlinkUsbWrite(USB_INTF_STREAM, data, length, SEND_TIMEOUT) == length;
    mutexUnlock(&sendMutex);

    return success;
}

/* 阻塞式握手:发 HELLO -> 收请求 -> 校验 -> 回响应 */
static bool connectClient(void)
{
    size_t num = 0;

    /* 循环发 HELLO 直到主机收到 */
    do
        num = swlinkUsbWrite(USB_INTF_STREAM, SWLINK_HELLO, sizeof(SWLINK_HELLO), HANDSHAKE_TIMEOUT);
    while (num == 0 && g_streamRunning);

    if (num != sizeof(SWLINK_HELLO) || !g_streamRunning)
        return false;

    SwlinkHandshakeRequest req;
    if (swlinkUsbRead(USB_INTF_STREAM, &req, sizeof(req), HANDSHAKE_TIMEOUT) != sizeof(req))
        return false;

    SwlinkHandshakeResponse resp = { 0 };

    if (req.magic != SWLINK_HANDSHAKE_MAGIC)
        resp.code = SWLINK_HANDSHAKE_BAD_MAGIC;
    else if (req.proto_ver != ('0' | ('1' << 8)))
        resp.code = SWLINK_HANDSHAKE_BAD_VERSION;
    else
        resp.code = SWLINK_HANDSHAKE_OK;

    if (swlinkUsbWrite(USB_INTF_STREAM, &resp, sizeof(resp), HANDSHAKE_TIMEOUT) != sizeof(resp))
        return false;

    if (resp.code != SWLINK_HANDSHAKE_OK)
        return false;

    videoConnected = req.want_video != 0;
    audioConnected = req.want_audio != 0;

    if (!clientConnected())
        return false;

    captureSetAudioBatching(req.audio_batching);

    if (videoConnected)
        captureVideoConnected();
    if (audioConnected)
        captureAudioConnected();

    return true;
}

void streamVideoThread(void* arg)
{
    (void)arg;

    while (g_streamRunning) {
        if (!clientConnected()) {
            /* 视频线程作为主线程负责握手 */
            if (!connectClient()) {
                svcSleepThread(5E+8L);
                continue;
            }
        } else if (videoConnected) {
            /* grc 失败也会产出错误包,照常发送 */
            captureReadVideo();

            if (!g_streamRunning)
                continue;

            if (!sendPacket(&VPkt, VPkt.header.data_size + sizeof(SwlinkStreamHeader))) {
                disconnectClient();
                svcSleepThread(5E+8L);
            }
        } else {
            /* 客户端只要音频 */
            svcSleepThread(1E+9L);
        }
    }
}

void streamAudioThread(void* arg)
{
    (void)arg;

    while (g_streamRunning) {
        if (audioConnected) {
            captureReadAudio();

            if (!g_streamRunning)
                continue;

            if (!sendPacket(&APkt, APkt.header.data_size + sizeof(SwlinkStreamHeader))) {
                disconnectClient();
                svcSleepThread(5E+8L);
            }
        } else {
            svcSleepThread(1E+9L);
        }
    }
}
