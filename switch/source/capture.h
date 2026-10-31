/*
 * capture.h
 * grc:d 视频(H.264) + 音频(PCM) 采集
 */
#pragma once
#include <switch.h>
#include "../../protocol/swlink_protocol.h"

typedef struct {
    SwlinkStreamHeader header;
    u8 data[SWLINK_VIDEO_BUF_SIZE];
} VideoPacket;

typedef struct {
    SwlinkStreamHeader header;
    u8 data[SWLINK_AUDIO_BUF_SIZE];
} AudioPacket;

/* 静态包缓冲,采集线程填充后由发送线程取走 */
extern VideoPacket VPkt;
extern AudioPacket APkt;

/* 打开 grc:d 的两个 session (视频/音频) */
Result captureInitialize(void);
void captureFinalize(void);

/* 新客户端连接后调用:重置状态,强制下一帧注入 SPS/PPS */
void captureVideoConnected(void);
void captureAudioConnected(void);

/* 设置音频批量块数 0..SWLINK_AUDIO_MAX_BATCH */
void captureSetAudioBatching(int batch);

/*
 * 读一帧视频/音频到 VPkt/APkt。
 * 成功返回 true;失败返回 false,此时 header.flags 置 ERROR 位、
 * payload 为 u32 Result 错误码 (仍可发送给客户端用于诊断)。
 * 注意:主菜单或不支持录制的游戏会无限阻塞,属正常现象。
 */
bool captureReadVideo(void);
bool captureReadAudio(void);
