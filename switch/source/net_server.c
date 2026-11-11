/*
 * net_server.c
 * 无线串流接入:
 *   1) TCP 监听 SWLINK_TCP_PORT,主机连上后按与 USB 相同的顺序握手;
 *   2) UDP 监听 SWLINK_DISCOVER_PORT,回复本机串流端口,响应源 IP 即设备地址。
 *
 * 握手成功后的包发送由 stream_server.c 负责(fd 所有权移交给它)。
 * 本文件只做「接入」和「连接存活检测」,不做任何堆分配。
 */
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <switch/services/bsd.h>

#include "net_server.h"
#include "stream_server.h"
#include "../../protocol/swlink_protocol.h"

extern void sdLog(const char* text);
extern void sdLogRc(const char* tag, Result rc);

#define NET_STACK_SIZE  0x8000
#define NET_THREAD_PRIO 0x2C

static volatile bool g_netRunning = false;
static bool g_sockReady = false;
static Mutex g_sockMutex;

static Thread g_acceptThread;
static Thread g_senderThread;
static Thread g_discoveryThread;

static bool ensureSocket(void)
{
    mutexLock(&g_sockMutex);
    if (!g_sockReady) {
        /* sysmodule(AppletType_None)没有 bsd:u 的使用权限,必须走 bsd:s;
           Auto 会先试 bsd:s,失败再回退 bsd:u。
           BSD 的 transfer memory 从本进程 2MB 堆里分配,缓冲参数必须调小,
           否则 tmemCreate 会因内存不足失败。 */
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.tcp_tx_buf_size = 0x8000;
        cfg.tcp_rx_buf_size = 0x8000;
        cfg.tcp_tx_buf_max_size = 0x20000;
        cfg.tcp_rx_buf_max_size = 0x20000;
        cfg.udp_tx_buf_size = 0x2400;
        cfg.udp_rx_buf_size = 0x2400;
        cfg.sb_efficiency = 2;
        cfg.bsd_service_type = BsdServiceType_Auto;

        static bool started = false;
        if (!started) {
            started = true;
            sdLog("socketInitialize 开始");
        }

        Result rc = socketInitialize(&cfg);
        if (R_SUCCEEDED(rc)) {
            g_sockReady = true;
            sdLog("socket 初始化成功 (bsd:s)");
        } else {
            static bool logged = false;
            if (!logged) {
                logged = true;
                sdLogRc("socket 初始化失败", rc);
            }
        }
    }
    bool ready = g_sockReady;
    mutexUnlock(&g_sockMutex);
    return ready;
}

static bool sendAll(int fd, const void* data, size_t len)
{
    const u8* p = (const u8*)data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool recvAll(int fd, void* data, size_t len)
{
    u8* p = (u8*)data;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void setTimeouts(int fd, int recvSec, int sendMs)
{
    struct timeval rv = { .tv_sec = recvSec, .tv_usec = 0 };
    struct timeval sv = { .tv_sec = sendMs / 1000, .tv_usec = (sendMs % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rv, sizeof(rv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sv, sizeof(sv));
}

/* Horizon BSD 的 fcntl 常量沿用 BSD ABI:F_SETFL=4,O_NONBLOCK=4。
   成功后会话 socket 的 send/recv 都不会再阻塞。 */
static void trySetNonBlocking(int fd)
{
    Result rc = (Result)bsdFcntl(fd, 4, 4);
    sdLogRc("无线 socket 非阻塞", rc);
}

/* 与 USB 版 stream_server.c 相同的握手顺序:发 HELLO -> 读请求 -> 回响应。
   成功后把 fd 交给 stream_server 发送线程。 */
static bool doHandshake(int fd)
{
    SwlinkHandshakeRequest req;
    SwlinkHandshakeResponse resp = { 0 };

    if (!sendAll(fd, SWLINK_HELLO, sizeof(SWLINK_HELLO)))
        return false;
    if (!recvAll(fd, &req, sizeof(req)))
        return false;

    if (req.magic != SWLINK_HANDSHAKE_MAGIC) {
        resp.code = SWLINK_HANDSHAKE_BAD_MAGIC;
    } else if (req.proto_ver != ('0' | ('1' << 8))) {
        resp.code = SWLINK_HANDSHAKE_BAD_VERSION;
    } else {
        resp.code = SWLINK_HANDSHAKE_OK;
    }

    if (!sendAll(fd, &resp, sizeof(resp)))
        return false;

    sdLogRc("无线握手 magic", req.magic);
    sdLogRc("无线握手响应码 (0=成功)", resp.code);
    sdLogRc("无线握手需求 video|audio<<8", (Result)(req.want_video | (req.want_audio << 8)));

    if (resp.code != SWLINK_HANDSHAKE_OK)
        return false;

    if (!req.want_video && !req.want_audio)
        return false;

    return streamServerNetStage(fd, req.want_video != 0, req.want_audio != 0,
                                req.audio_batching);
}

static void acceptThread(void* arg)
{
    (void)arg;

    sdLog("TCP 线程已进入");

    int one = 1;

    while (g_netRunning) {
        if (!ensureSocket()) {
            svcSleepThread(3E+9L);
            continue;
        }

        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            static int logged = 0;
            if (logged++ < 3)
                sdLogRc("TCP socket 失败 errno", (Result)errno);
            svcSleepThread(1E+9L);
            continue;
        }

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(SWLINK_TCP_PORT),
            .sin_addr.s_addr = INADDR_ANY,
        };

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
            static int logged = 0;
            if (logged++ < 3)
                sdLogRc("TCP bind/listen 失败 errno", (Result)errno);
            close(fd);
            svcSleepThread(3E+9L);
            continue;
        }

        /* 监听 socket 也设超时,accept 才能周期性回到循环检查退出标志
           (不要在别的线程 close 它,避免阻塞中的 accept 返回后二次 close) */
        setTimeouts(fd, 1, 1000);
        sdLogRc("TCP 监听", SWLINK_TCP_PORT);

        while (g_netRunning) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int cfd = accept(fd, (struct sockaddr*)&peer, &plen);

            if (cfd < 0) {
                if (!g_netRunning)
                    break;
                continue;
            }

            one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            /* 握手阶段用阻塞超时,成功后再交给发送线程 */
            setTimeouts(cfd, 5, 5000);

            if (!doHandshake(cfd)) {
                sdLog("无线握手失败, 断开");
                close(cfd);
                continue;
            }

            setTimeouts(cfd, 1, 20);
            trySetNonBlocking(cfd);
            sdLog("无线客户端已接入, 等待发送线程接管");
        }

        close(fd);
    }
}

/* 发送线程:独占当前会话 fd,负责发包和探测断开。
   新会话由 accept 线程暂存,这里关闭旧会话后接管。 */
static void senderThread(void* arg)
{
    (void)arg;

    sdLog("无线发送线程已进入");

    while (g_netRunning) {
        bool wantVideo = false;
        bool wantAudio = false;
        uint8_t batching = 0;

        int pending = streamServerNetTakePending(&wantVideo, &wantAudio, &batching);
        if (pending >= 0) {
            sdLogRc("发送线程取到会话 fd", (Result)pending);
            streamServerDetachNet(-1);
            if (streamServerAttachNet(pending, wantVideo, wantAudio, batching)) {
                sdLog("无线会话已接管,开始串流");
            } else {
                sdLog("无线会话接管失败");
                close(pending);
            }
            continue;
        }

        int fd = streamServerNetFd();
        if (fd < 0) {
            svcSleepThread(5E+6L);
            continue;
        }

        if (streamServerNetPending()) {
            if (!streamServerNetFlush(fd)) {
                sdLog("无线发送失败, 断开");
                streamServerDetachNet(-1);
            } else if (streamServerNetPending()) {
                svcSleepThread(1E+6L);
            }
        } else {
            /* 没有数据要发时短暂让出 CPU;对端断开由发送失败检测 */
            svcSleepThread(2E+6L);
        }
    }

    streamServerDetachNet(-1);
}

static void discoveryThread(void* arg)
{
    (void)arg;

    sdLog("UDP 线程已进入");

    int one = 1;

    while (g_netRunning) {
        if (!ensureSocket()) {
            svcSleepThread(3E+9L);
            continue;
        }

        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) {
            static int logged = 0;
            if (logged++ < 3)
                sdLogRc("UDP socket 失败 errno", (Result)errno);
            svcSleepThread(1E+9L);
            continue;
        }

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        setTimeouts(fd, 1, 1000);

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(SWLINK_DISCOVER_PORT),
            .sin_addr.s_addr = INADDR_ANY,
        };

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            static int logged = 0;
            if (logged++ < 3)
                sdLogRc("UDP bind 失败 errno", (Result)errno);
            close(fd);
            svcSleepThread(3E+9L);
            continue;
        }

        sdLogRc("UDP 发现监听", SWLINK_DISCOVER_PORT);

        while (g_netRunning) {
            SwlinkDiscoveryMsg req;
            struct sockaddr_in from;
            socklen_t flen = sizeof(from);

            ssize_t n = recvfrom(fd, &req, sizeof(req), 0,
                                 (struct sockaddr*)&from, &flen);
            if (n != (ssize_t)sizeof(req) || !g_netRunning)
                continue;
            if (memcmp(req.magic, SWLINK_DISCOVER_REQ, sizeof(req.magic)) != 0)
                continue;

            SwlinkDiscoveryMsg resp;
            memset(&resp, 0, sizeof(resp));
            memcpy(resp.magic, SWLINK_DISCOVER_RESP, sizeof(SWLINK_DISCOVER_RESP));
            resp.tcp_port = SWLINK_TCP_PORT;
            resp.proto_ver = ('1' << 8) | '0';

            sendto(fd, &resp, sizeof(resp), 0, (struct sockaddr*)&from, flen);
        }

        close(fd);
    }
}

void netServerInit(void)
{
    if (g_netRunning)
        return;

    g_netRunning = true;
    mutexInit(&g_sockMutex);

    static u8 acceptStack[NET_STACK_SIZE] __attribute__((aligned(0x1000)));
    static u8 senderStack[NET_STACK_SIZE] __attribute__((aligned(0x1000)));
    static u8 discoveryStack[NET_STACK_SIZE] __attribute__((aligned(0x1000)));

    Result rc = threadCreate(&g_acceptThread, acceptThread, NULL,
                             acceptStack, sizeof(acceptStack), NET_THREAD_PRIO, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_acceptThread);
    sdLogRc("无线 TCP 线程", rc);

    rc = threadCreate(&g_senderThread, senderThread, NULL,
                      senderStack, sizeof(senderStack), NET_THREAD_PRIO, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_senderThread);
    sdLogRc("无线发送线程", rc);

    rc = threadCreate(&g_discoveryThread, discoveryThread, NULL,
                      discoveryStack, sizeof(discoveryStack), NET_THREAD_PRIO, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_discoveryThread);
    sdLogRc("无线发现线程", rc);
}

void netServerExit(void)
{
    if (!g_netRunning)
        return;

    g_netRunning = false;

    /* 关闭当前无线会话(若有);监听线程会在超时后自行退出 */
    streamServerDetachNet(-1);
}
