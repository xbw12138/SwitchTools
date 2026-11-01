/*
 * stream_server.c
 * 串流服务实现:HELLO 握手,视频/音频复用同一 IN 端点
 */
#include <string.h>
#include "stream_server.h"
#include "capture.h"
#include "usb_link.h"
#include "../../protocol/swlink_protocol.h"

/* 来自 main.c 的日志器 */
extern void sdLog(const char* text);
extern void sdLogRc(const char* tag, Result rc);

/* 记录写入的实际/期望字节数,便于区分"主机没读"和"写超时" */
static void logWriteFail(size_t written, size_t expected)
{
    static int n = 0;
    if (n++ >= 5)
        return;
    sdLogRc("USB 写入 实际/期望", (Result)((written << 16) | (expected & 0xFFFF)));
}

#define HANDSHAKE_TIMEOUT 1E+9  /* 1s */
/* 100ms 足够传完最大视频包;设太小会误丢帧,设成 1s 一旦主机暂时没读就会卡顿整秒 */
#define SEND_TIMEOUT      1E+8

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
    if (!clientConnected()) {
        success = false;
    } else {
        size_t written = swlinkUsbWrite(USB_INTF_STREAM, data, length, SEND_TIMEOUT);
        if (written != length)
            logWriteFail(written, length);
        success = written == length;
    }
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

    sdLogRc("握手请求 magic", req.magic);
    sdLogRc("握手响应码 (0=成功)", resp.code);
    sdLogRc("握手需求 video|audio<<8", (Result)(req.want_video | (req.want_audio << 8)));

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

    int consecutiveFails = 0;

    while (g_streamRunning) {
        if (!clientConnected()) {
            /* 视频线程作为主线程负责握手 */
            if (!connectClient()) {
                svcSleepThread(5E+8L);
                continue;
            }
            consecutiveFails = 0;
        } else if (videoConnected) {
            /* grc 失败也会产出错误包,照常发送 */
            captureReadVideo();

            if (!g_streamRunning)
                continue;

            if (sendPacket(&VPkt, VPkt.header.data_size + sizeof(SwlinkStreamHeader))) {
                consecutiveFails = 0;
            } else {
                /* 偶发写超时(主机瞬间没在读)只丢这一帧;下个 IDR 重新带上 SPS/PPS,
                   连续多次失败才判定掉线,避免一次卡顿就断开串流 */
                captureVideoConnected();
                if (++consecutiveFails >= 10) {
                    disconnectClient();
                    consecutiveFails = 0;
                    svcSleepThread(5E+8L);
                }
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

    int consecutiveFails = 0;

    while (g_streamRunning) {
        if (audioConnected) {
            captureReadAudio();

            if (!g_streamRunning)
                continue;

            if (sendPacket(&APkt, APkt.header.data_size + sizeof(SwlinkStreamHeader))) {
                consecutiveFails = 0;
            } else if (++consecutiveFails >= 20) {
                /* 音频可丢弃,连续失败才断线 */
                disconnectClient();
                consecutiveFails = 0;
                svcSleepThread(5E+8L);
            }
        } else {
            svcSleepThread(1E+9L);
        }
    }
}
