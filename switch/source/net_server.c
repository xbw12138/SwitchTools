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
#include "capture.h"
#include "file_server.h"
#include "../../protocol/swlink_protocol.h"

extern void sdLog(const char* text);
extern void sdLogRc(const char* tag, Result rc);

#define NET_STACK_SIZE  0x8000
#define NET_THREAD_PRIO 0x2C

static volatile bool g_netRunning = false;
static bool g_sockReady = false;
static Mutex g_sockMutex;

static Thread g_acceptThread;
static Thread g_discoveryThread;
static Thread g_fileThread;

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
        cfg.tcp_tx_buf_max_size = 0x40000;
        cfg.tcp_rx_buf_max_size = 0x40000;
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

static void serveNetSession(int fd);

/* 与 USB 版 stream_server.c 相同的握手顺序:发 HELLO -> 读请求 -> 回响应。
   成功时把客户端需求写回调用方,由接入线程自己接管发送。 */
static bool doHandshake(int fd, bool* wantVideo, bool* wantAudio, uint8_t* batching)
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
    } else if (R_FAILED(captureEnsureInitialized())) {
        resp.code = SWLINK_HANDSHAKE_BAD_REQUEST;
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

    *wantVideo = req.want_video != 0;
    *wantAudio = req.want_audio != 0;
    *batching = req.audio_batching;
    return true;
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
        static int listenLogged = 0;
        if (listenLogged++ < 3)
            sdLogRc("TCP 监听", SWLINK_TCP_PORT);

        while (g_netRunning) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int cfd = accept(fd, (struct sockaddr*)&peer, &plen);

            if (cfd < 0) {
                if (!g_netRunning)
                    break;
                /* 正常超时:继续等待。其他错误(通常是休眠唤醒后句柄失效)
                   必须退避并重建监听 socket,否则会变成 100% 忙等拖死系统。 */
                if (errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                static int errLog = 0;
                if (errLog++ < 5)
                    sdLogRc("TCP accept 异常 errno", (Result)errno);
                svcSleepThread(2E+8L);
                break;
            }

            one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            /* 握手阶段用阻塞超时,成功后再交给发送线程 */
            setTimeouts(cfd, 5, 5000);

            bool wantVideo = false;
            bool wantAudio = false;
            uint8_t batching = 0;

            if (!doHandshake(cfd, &wantVideo, &wantAudio, &batching)) {
                sdLog("无线握手失败, 断开");
                close(cfd);
                continue;
            }

            /* 与文件通道保持一致:保持阻塞 + 3s 发送超时,由专用发送线程背压。
               原来的非阻塞 + 20ms 超时会在高码率下反复 EAGAIN,导致发送线程
               空转、队列被丢包,表现为画面只有几帧每秒。 */
            setTimeouts(cfd, 1, 3000);

            /* 显式放大发送缓冲,并打印实际生效值用于诊断 */
            {
                int want = 0x40000;
                setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &want, sizeof(want));
                int got = 0;
                socklen_t gl = sizeof(got);
                getsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &got, &gl);
                sdLogRc("串流 SO_SNDBUF", (Result)got);
            }
            sdLog("无线客户端已接入, 本线程开始发送");

            /* 会话由本线程创建,也由本线程发送,避免跨线程阻塞 send */
            streamServerDetachNet(-1);
            if (!streamServerAttachNet(cfd, wantVideo, wantAudio, batching)) {
                sdLog("无线会话接管失败");
                close(cfd);
                continue;
            }
            serveNetSession(cfd);
        }

        close(fd);
    }
}

/* 在当前线程里发送整条会话。
   套接字由 accept 线程创建,就必须由同一线程做阻塞 send:
   实测跨线程发送时每条新连接只有 ~50KB/s,而同线程的文件通道可达 1MB/s。 */
static void serveNetSession(int fd)
{
    sdLog("无线会话已接管,开始串流");

    static u64 lastTick = 0;
    static u64 lastBytes = 0;
    static u32 lastCalls = 0;
    static u64 lastSendUs = 0;

    while (g_netRunning) {
        /* 一次尽量排空队列:每片 32KB,连续发 8 片再让出 CPU */
        bool ok = true;
        int guard = 0;
        do {
            ok = streamServerNetFlush(fd);
            guard++;
        } while (ok && guard < 8 && streamServerNetPending());

        if (!ok) {
            sdLog("无线发送失败, 断开");
            break;
        }

        if (streamServerNetPending()) {
            svcSleepThread(1E+6L);
        } else {
            /* 没数据要发时读一次心跳,顺便发现对端已关闭 */
            if (!streamServerNetRead(fd)) {
                sdLog("无线会话已断开");
                break;
            }
            svcSleepThread(2E+6L);
        }

        /* 每 30 秒打印一次发送统计,便于定位卡顿 */
        u64 now = svcGetSystemTick();
        if (lastTick == 0)
            lastTick = now;
        if (now - lastTick >= 30ULL * 19200000ULL) {
            unsigned drops = 0;
            u64 bytes = 0;
            u32 calls = 0, maxUs = 0, backlog = 0;
            u64 sendUs = 0;
            streamServerNetStats(&drops, &bytes);
            streamServerNetDiag(&calls, &sendUs, &maxUs, &backlog);

            /* 会话重建时计数会归零,避免出现巨大负数 */
            if (bytes < lastBytes) { lastBytes = bytes; lastCalls = calls; lastSendUs = sendUs; }

            const u32 kbs = (u32)((bytes - lastBytes) / 30 / 1024);
            const u32 dcalls = calls - lastCalls;
            const u32 avgMs = dcalls ? (u32)((sendUs - lastSendUs) / dcalls / 1000) : 0;

            sdLogRc("无线发送 KB/s", kbs);
            sdLogRc("无线丢包累计", drops);
            sdLogRc("无线 send 次数/5s", dcalls);
            sdLogRc("无线 send 平均 ms", avgMs);
            sdLogRc("无线 send 最大 ms", maxUs);
            sdLogRc("无线队列积压 KB", backlog / 1024);

            lastTick = now;
            lastBytes = bytes;
            lastCalls = calls;
            lastSendUs = sendUs;
        }
    }

    streamServerDetachNet(fd);
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

            if (n < 0) {
                if (!g_netRunning)
                    break;
                if (errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                static int errLog = 0;
                if (errLog++ < 5)
                    sdLogRc("UDP recv 异常 errno", (Result)errno);
                svcSleepThread(2E+8L);
                break;
            }
            if (n != (ssize_t)sizeof(req))
                continue;
            if (memcmp(req.magic, SWLINK_DISCOVER_REQ, sizeof(req.magic)) != 0)
                continue;

            SwlinkDiscoveryMsg resp;
            memset(&resp, 0, sizeof(resp));
            memcpy(resp.magic, SWLINK_DISCOVER_RESP, sizeof(SWLINK_DISCOVER_RESP));
            resp.tcp_port = SWLINK_TCP_PORT;
            resp.proto_ver = ('1' << 8) | '0';
            resp.ft_port = SWLINK_FT_TCP_PORT;

            sendto(fd, &resp, sizeof(resp), 0, (struct sockaddr*)&from, flen);
        }

        close(fd);
    }
}

/* 文件通道:独立端口,连上后交给 file_server 循环处理请求。
   与串流端口分开监听,互不影响;同一时刻只服务一个客户端。 */
static void fileAcceptThread(void* arg)
{
    (void)arg;

    sdLog("TCP 文件线程已进入");

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
                sdLogRc("文件 socket 失败 errno", (Result)errno);
            svcSleepThread(1E+9L);
            continue;
        }

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(SWLINK_FT_TCP_PORT),
            .sin_addr.s_addr = INADDR_ANY,
        };

        if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 || listen(fd, 2) != 0) {
            static int logged = 0;
            if (logged++ < 3)
                sdLogRc("文件 bind/listen 失败 errno", (Result)errno);
            close(fd);
            svcSleepThread(3E+9L);
            continue;
        }

        setTimeouts(fd, 1, 1000);
        static int listenLogged = 0;
        if (listenLogged++ < 3)
            sdLogRc("TCP 文件监听", SWLINK_FT_TCP_PORT);

        while (g_netRunning) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            int cfd = accept(fd, (struct sockaddr*)&peer, &plen);

            if (cfd < 0) {
                if (!g_netRunning)
                    break;
                if (errno == ETIMEDOUT || errno == EAGAIN ||
                    errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                static int errLog = 0;
                if (errLog++ < 5)
                    sdLogRc("文件 accept 异常 errno", (Result)errno);
                svcSleepThread(2E+8L);
                break;
            }

            one = 1;
            setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            /* recv/send 超时:1s 切片,让 readFull 能周期性检查退出标志 */
            setTimeouts(cfd, 1, 3000);
            {
                int got = 0;
                socklen_t gl = sizeof(got);
                getsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &got, &gl);
                sdLogRc("文件 SO_SNDBUF", (Result)got);
            }

            sdLog("文件客户端已接入 (TCP)");
            fileServerNetServe(cfd);
            close(cfd);
            sdLog("文件客户端已断开 (TCP)");
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
    static u8 discoveryStack[NET_STACK_SIZE] __attribute__((aligned(0x1000)));
    static u8 fileStack[NET_STACK_SIZE] __attribute__((aligned(0x1000)));

    Result rc = threadCreate(&g_acceptThread, acceptThread, NULL,
                             acceptStack, sizeof(acceptStack), NET_THREAD_PRIO, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_acceptThread);
    sdLogRc("无线 TCP 线程", rc);

    rc = threadCreate(&g_discoveryThread, discoveryThread, NULL,
                      discoveryStack, sizeof(discoveryStack), NET_THREAD_PRIO, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_discoveryThread);
    sdLogRc("无线发现线程", rc);

    rc = threadCreate(&g_fileThread, fileAcceptThread, NULL,
                      fileStack, sizeof(fileStack), NET_THREAD_PRIO, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&g_fileThread);
    sdLogRc("无线文件线程", rc);
}

void netServerExit(void)
{
    if (!g_netRunning)
        return;

    g_netRunning = false;

    /* 关闭当前无线会话(若有);监听线程会在超时后自行退出 */
    streamServerDetachNet(-1);
}
