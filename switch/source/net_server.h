/*
 * net_server.h
 * 局域网无线串流:TCP 串流端口 + UDP 设备发现
 */
#pragma once
#include <switch.h>

/* 启动无线服务(两个后台线程:TCP 接入 / UDP 发现)。
   网络尚未就绪时内部会自行重试,不影响 USB 串流启动。 */
void netServerInit(void);

/* 停止服务并关闭监听 socket */
void netServerExit(void);
