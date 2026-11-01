/*
 * capture.c
 * grc:d 采集实现 (参考 SysDVR capture.c,去掉 NAL 哈希去重)
 */
#include <string.h>
#include "capture.h"

/* 来自 main.c 的日志器 (sysmodule 没有控制台) */
extern void sdLog(const char* text);
extern void sdLogRc(const char* tag, Result rc);

VideoPacket alignas(0x1000) VPkt;
AudioPacket alignas(0x1000) APkt;

static Service grcdVideo;
static Service grcdAudio;

/* 静态 SPS/PPS:grc 只在游戏启动时输出一次,需周期性在 IDR 前补发 */
static const u8 SPS[] = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x0C, 0x20, 0xAC, 0x2B, 0x40, 0x28, 0x02, 0xDD, 0x35, 0x01, 0x0D, 0x01, 0xE0, 0x80 };
static const u8 PPS[] = { 0x00, 0x00, 0x00, 0x01, 0x68, 0xEE, 0x3C, 0xB0 };

/* Begin 全局每开机只能调一次,重复调用会让主机崩溃。
   策略同 SysDVR:先 Transfer,返回 0x3E8D4 (2212-0500) 说明未 Begin,再补调。 */
#define GRCD_NOT_INITIALIZED 0x3E8D4
static bool grcdBeginCalled = false;
static Mutex grcdBeginMutex;

static bool forceSPSPPS = false;
static int audioBatching = 3;


static Result grcBeginCmd(Service* srv, u32 cmd_id)
{
    return serviceDispatch(srv, cmd_id);
}

static Result grcTransferCmd(Service* srv, u32 stream, void* buffer, size_t size, u32* data_size, u64* start_timestamp)
{
    struct {
        u32 num_frames;
        u32 data_size;
        u64 start_timestamp;
    } out;

    Result rc = serviceDispatchInOut(srv, 2, stream, out,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, size } });

    if (R_SUCCEEDED(rc)) {
        if (data_size) *data_size = out.data_size;
        if (start_timestamp) *start_timestamp = out.start_timestamp;
    }
    return rc;
}

/* 返回 true 表示本次确实调用了 Begin;rc 为 Begin 的结果 */
static bool ensureGrcdInit(Result* rc)
{
    mutexLock(&grcdBeginMutex);

    if (grcdBeginCalled) {
        mutexUnlock(&grcdBeginMutex);
        return false;
    }

    grcdBeginCalled = true;
    *rc = grcBeginCmd(&grcdVideo, 1);
    sdLogRc("grc:d Begin", *rc);

    mutexUnlock(&grcdBeginMutex);
    return true;
}

static void packetMakeError(SwlinkStreamHeader* header, void* payload, u8 type, Result rc)
{
    header->flags |= SWLINK_STREAM_FLAG_ERROR;
    header->type = type;
    header->data_size = sizeof(u32);
    memcpy(payload, &rc, sizeof(u32));
}

Result captureInitialize(void)
{
    Result rc;

    if (!serviceIsActive(&grcdVideo)) {
        rc = smGetService(&grcdVideo, "grc:d");
        if (R_FAILED(rc)) return rc;
    }
    if (!serviceIsActive(&grcdAudio)) {
        rc = smGetService(&grcdAudio, "grc:d");
        if (R_FAILED(rc)) {
            serviceClose(&grcdVideo);
            return rc;
        }
    }
    return 0;
}

void captureFinalize(void)
{
    serviceClose(&grcdVideo);
    serviceClose(&grcdAudio);
}

void captureVideoConnected(void)
{
    VPkt.header.magic = SWLINK_PKT_MAGIC;
    VPkt.header.type = SwlinkStream_Video;
    VPkt.header.flags = 0;
    forceSPSPPS = true;
}

void captureAudioConnected(void)
{
    APkt.header.magic = SWLINK_PKT_MAGIC;
    APkt.header.type = SwlinkStream_Audio;
    APkt.header.flags = 0;
}

void captureSetAudioBatching(int batch)
{
    if (batch < 0) batch = 0;
    if (batch > SWLINK_AUDIO_MAX_BATCH) batch = SWLINK_AUDIO_MAX_BATCH;
    audioBatching = batch;
}

bool captureReadVideo(void)
{
    u32 dataSize = 0;
    u64 timestamp = 0;

    Result rc = grcTransferCmd(&grcdVideo, GrcStream_Video,
        VPkt.data, SWLINK_VIDEO_BUF_SIZE, &dataSize, &timestamp);

    if (rc == GRCD_NOT_INITIALIZED && ensureGrcdInit(&rc)) {
        if (R_FAILED(rc)) {
            packetMakeError(&VPkt.header, VPkt.data, SwlinkStream_Video, rc);
            return false;
        }
        return captureReadVideo();
    }

    VPkt.header.data_size = dataSize;
    VPkt.header.timestamp = timestamp;
    VPkt.header.flags = 0;

    /* 运行时探针:前 5 帧记录 大小/NAL 类型,之后每 100 帧记录累计帧数与最近大小 */
    static int okLog = 0, failLog = 0, totalFrames = 0;
    if (R_SUCCEEDED(rc) && dataSize > 4) {
        const u8 nalType = VPkt.data[4] & 0x1F;
        totalFrames++;
        if (okLog < 5) {
            okLog++;
            /* 高 24 位 = 帧大小, 低 8 位 = NAL 类型 (1=普通帧 5=IDR 7=SPS 8=PPS) */
            sdLogRc("视频帧 大小<<8|NAL", (Result)(((u32)dataSize << 8) | nalType));
        } else if (totalFrames % 100 == 0) {
            sdLogRc("累计视频帧数", (Result)totalFrames);
            sdLogRc("最近帧大小", (Result)dataSize);
        }
    } else if (failLog < 6) {
        failLog++;
        sdLogRc("视频 Transfer rc", rc);
        sdLogRc("视频数据量", (Result)dataSize);
    }

    /* dataSize <= 4 视为无效;0xcd4 (2212-0006) 表示缓冲过小,常见于超大 IDR */
    if (!R_SUCCEEDED(rc) || dataSize <= 4) {
        packetMakeError(&VPkt.header, VPkt.data, SwlinkStream_Video, R_SUCCEEDED(rc) ? MAKERESULT(2212, 6) : rc);
        return false;
    }

    /* grc 每包只有一个 NAL,IDR 判定直接看第 5 字节 */
    const bool isIDRFrame = (VPkt.data[4] & 0x1F) == 5;

    /* 每 5 个 IDR(或强制)在包首补 SPS/PPS,保证客户端中途接入可解码 */
    static int idrCount = 0;
    bool emitMeta = forceSPSPPS || (isIDRFrame && ++idrCount >= 5);

    if (emitMeta && (SWLINK_VIDEO_BUF_SIZE - dataSize) >= (sizeof(SPS) + sizeof(PPS))) {
        idrCount = 0;
        forceSPSPPS = false;
        memmove(VPkt.data + sizeof(SPS) + sizeof(PPS), VPkt.data, dataSize);
        memcpy(VPkt.data, SPS, sizeof(SPS));
        memcpy(VPkt.data + sizeof(SPS), PPS, sizeof(PPS));
        VPkt.header.data_size += sizeof(SPS) + sizeof(PPS);
        VPkt.header.flags |= SWLINK_STREAM_FLAG_SPSPPS;
    }

    return true;
}

bool captureReadAudio(void)
{
    u32 dataSize = 0;
    u64 timestamp = 0;

    Result rc = grcTransferCmd(&grcdAudio, GrcStream_Audio,
        APkt.data, SWLINK_AUDIO_CHUNK, &dataSize, &timestamp);

    if (rc == GRCD_NOT_INITIALIZED && ensureGrcdInit(&rc)) {
        if (R_FAILED(rc)) {
            packetMakeError(&APkt.header, APkt.data, SwlinkStream_Audio, rc);
            return false;
        }
        return captureReadAudio();
    }

    APkt.header.data_size = dataSize;
    APkt.header.timestamp = timestamp;
    APkt.header.flags = 0;

    if (!R_SUCCEEDED(rc)) {
        packetMakeError(&APkt.header, APkt.data, SwlinkStream_Audio, rc);
        return false;
    }

    /* 批量追加若干块,降低 USB 事务数 */
    for (int i = 0; i < audioBatching; i++) {
        u32 tmpSize = 0;

        rc = grcTransferCmd(&grcdAudio, GrcStream_Audio,
            APkt.data + APkt.header.data_size, SWLINK_AUDIO_CHUNK, &tmpSize, NULL);

        if (!R_SUCCEEDED(rc)) {
            packetMakeError(&APkt.header, APkt.data, SwlinkStream_Audio, rc);
            return false;
        }

        APkt.header.data_size += tmpSize;
    }

    return true;
}
