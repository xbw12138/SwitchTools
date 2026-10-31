/*
 * stream_server.h
 * 串流服务:握手 + 视频/音频包发送 (共用流接口 IN 端点)
 */
#pragma once
#include <switch.h>

/* 运行标志由 main 控制,置 false 后线程尽快退出 */
extern volatile bool g_streamRunning;

/* 视频线程入口 (在 main 线程上跑) */
void streamVideoThread(void* arg);

/* 音频线程入口 (独立线程) */
void streamAudioThread(void* arg);
