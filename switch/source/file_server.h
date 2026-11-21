/*
 * file_server.h
 * SD 卡文件传输服务:运行在独立线程,处理文件接口上的请求
 */
#pragma once
#include <switch.h>

extern volatile bool g_fileRunning;

/* 传入已打开的 SD 卡 FsFileSystem */
void fileServerInit(FsFileSystem* sd);

/* 线程入口:循环 读请求头 -> 读 payload -> 处理 -> 回响应 */
void fileServerThread(void* arg);

/* 局域网文件会话:在已建立的 TCP 连接上循环处理请求,返回即表示连接结束 */
void fileServerNetServe(int fd);
