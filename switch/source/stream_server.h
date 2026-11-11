/*
 * stream_server.h
 * 串流服务:握手 + 视频/音频包发送 (共用流接口 IN 端点)
 */
#pragma once
#include <switch.h>

/* 运行标志由 main 控制,置 false 后线程尽快退出 */
extern volatile bool g_streamRunning;

/* 初始化互斥锁,必须在启动任何线程前调用 */
void streamServerInit(void);

/* 视频线程入口 (在 main 线程上跑) */
void streamVideoThread(void* arg);

/* 音频线程入口 (独立线程) */
void streamAudioThread(void* arg);

/* USB 发送线程入口 (独立线程,独占 USB 流写入) */
void streamUsbThread(void* arg);

/* ---------------- 无线 TCP 会话 ----------------
 * 由 net_server.c 在握手成功后接入;同一时刻只允许一个无线会话。
 * fd 的所有权移交给本模块,由 net_server 的会话线程关闭。
 * 视频/音频线程只把包放进发送队列,实际 socket 写入由会话线程完成。 */
bool streamServerAttachNet(int fd, bool wantVideo, bool wantAudio, uint8_t audioBatching);
void streamServerDetachNet(int fd);
bool streamServerHasNetClient(void);

/* 会话线程使用的接口 */
int streamServerNetFd(void);
bool streamServerNetPending(void);      /* 队列里还有待发数据 */
bool streamServerNetFlush(int fd);      /* 发送一次队列数据,false=链路出错 */
bool streamServerNetRead(int fd);       /* 读取心跳,false=对端已关闭 */

/* 握手线程暂存新会话,发送线程取走并接管(避免跨线程关闭 fd) */
bool streamServerNetStage(int fd, bool wantVideo, bool wantAudio, uint8_t audioBatching);
int streamServerNetTakePending(bool* wantVideo, bool* wantAudio, uint8_t* audioBatching);
