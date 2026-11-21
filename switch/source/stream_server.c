/*
 * stream_server.c
 * 串流服务实现:USB / 无线 TCP 共用握手与发送逻辑
 */
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>

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
    sdLogRc("写入 实际/期望", (Result)((written << 16) | (expected & 0xFFFF)));
}

#define HANDSHAKE_TIMEOUT 1E+9  /* 1s */
/* USB:100ms 足够传完最大视频包;设太小会误丢帧,设成 1s 一旦主机暂时没读就会卡顿整秒 */
#define SEND_TIMEOUT_USB  1E+8

/* 无线发送队列:采集线程只入队,TCP 会话线程异步发送。
   队列满时整包丢弃,保证网络拥塞不会阻塞视频采集线程。 */
#define NET_TX_BUF_SIZE (1024 * 1024)
static u8 netTxBuf[NET_TX_BUF_SIZE];
static size_t netTxLen = 0;    /* 队列中的有效字节数 */
static size_t netTxSent = 0;   /* 已发出的字节数 */
static unsigned netDropped = 0;

/* 视频丢过包(队列满)后置位,直到重新从 IDR 起送。
   只在视频线程内读写,不需要加锁。 */
static bool videoResync = false;

/* 握手完成、等待发送线程接管的新会话。
   用单个原子槽位传递,发送线程在空闲路径上不碰任何互斥锁。 */
static volatile u64 pendingSlot = 0;

/* 供发送线程无锁读取的会话 fd 副本 */
static volatile int netFdPub = -1;

/* 发送线程独占的待发块:把数据从共享队列拷出来后立刻释放锁,
   避免 send 阻塞时把采集线程和握手线程一起卡住 */
#define NET_CHUNK_SIZE 0x8000
static u8 netChunk[NET_CHUNK_SIZE];
static size_t chunkLen = 0;
static size_t chunkSent = 0;

static bool videoConnected = false;
static bool audioConnected = false;
static Mutex queueMutex;  /* 只保护无线会话和发送队列,绝不包住 USB I/O */

/* USB 发送队列:采集线程只入队,USB 写由独立线程完成。
   这样即使主机不读导致 usbComms 卡死,也只卡住 USB 线程,
   不会拖住采集线程和无线通道。 */
#define USB_TX_BUF_SIZE (256 * 1024)
static u8 usbTxBuf[USB_TX_BUF_SIZE];
static size_t usbTxLen = 0;
static size_t usbTxSent = 0;
static Mutex usbQueueMutex;
static u8 usbChunk[NET_CHUNK_SIZE];

void streamServerInit(void)
{
    mutexInit(&queueMutex);
    mutexInit(&usbQueueMutex);
}

/* 无线 TCP 会话(同一时刻只允许一个) */
static int netFd = -1;
static bool netVideoConnected = false;
static bool netAudioConnected = false;

static bool clientConnected(void)
{
    if (netFdPub >= 0)
        return true;
    return videoConnected || audioConnected;
}

static bool videoWanted(void)
{
    return netFdPub >= 0 ? netVideoConnected : videoConnected;
}

static bool audioWanted(void)
{
    return netFdPub >= 0 ? netAudioConnected : audioConnected;
}

bool streamServerHasNetClient(void)
{
    return netFdPub >= 0;
}

int streamServerNetFd(void)
{
    return netFdPub;
}

bool streamServerNetPending(void)
{
    bool pending;
    mutexLock(&queueMutex);
    pending = netFd >= 0 && netTxLen > netTxSent;
    mutexUnlock(&queueMutex);
    return pending;
}

/* 握手线程只把新连接暂存,由发送线程在自己的上下文里关闭旧会话并接管,
   避免跨线程关闭正在使用的 fd。fd 用 +1 编码,0 表示空槽。 */
bool streamServerNetStage(int fd, bool wantVideo, bool wantAudio, uint8_t audioBatching)
{
    u64 v;
    u64 old;

    if (fd < 0 || (!wantVideo && !wantAudio))
        return false;

    v = (u64)(u32)(fd + 1);
    if (wantVideo) v |= 1ULL << 32;
    if (wantAudio) v |= 1ULL << 33;
    v |= (u64)audioBatching << 40;

    old = __atomic_exchange_n(&pendingSlot, v, __ATOMIC_ACQ_REL);
    if (old)
        close((int)(u32)(old & 0xFFFFFFFFu) - 1);
    return true;
}

int streamServerNetTakePending(bool* wantVideo, bool* wantAudio, uint8_t* audioBatching)
{
    u64 v = __atomic_exchange_n(&pendingSlot, 0, __ATOMIC_ACQ_REL);

    if (!v)
        return -1;
    *wantVideo = (v >> 32) & 1;
    *wantAudio = (v >> 33) & 1;
    *audioBatching = (uint8_t)(v >> 40);
    return (int)(u32)(v & 0xFFFFFFFFu) - 1;
}

bool streamServerAttachNet(int fd, bool wantVideo, bool wantAudio, uint8_t audioBatching)
{
    if (fd < 0 || (!wantVideo && !wantAudio))
        return false;

    mutexLock(&queueMutex);
    if (netFd >= 0) {
        mutexUnlock(&queueMutex);
        return false;
    }
    netFd = fd;
    netFdPub = fd;
    netVideoConnected = wantVideo;
    netAudioConnected = wantAudio;
    netTxLen = 0;
    netTxSent = 0;
    netDropped = 0;
    chunkLen = 0;
    chunkSent = 0;
    mutexUnlock(&queueMutex);

    captureSetAudioBatching(audioBatching);
    if (wantVideo)
        captureVideoConnected();
    if (wantAudio)
        captureAudioConnected();

    return true;
}

void streamServerDetachNet(int fd)
{
    mutexLock(&queueMutex);
    if (netFd >= 0 && (fd < 0 || fd == netFd)) {
        close(netFd);
        netFd = -1;
        netFdPub = -1;
        netVideoConnected = false;
        netAudioConnected = false;
        netTxLen = 0;
        netTxSent = 0;
    }
    mutexUnlock(&queueMutex);
}

/* 把队列里的数据发出去;返回 false 表示链路出错。
   send 在锁外执行,网络阻塞不会拖住采集线程或握手线程。 */
bool streamServerNetFlush(int fd)
{
    if (chunkSent == chunkLen) {
        chunkLen = 0;
        chunkSent = 0;

        mutexLock(&queueMutex);
        if (netFd == fd && netTxSent < netTxLen) {
            size_t n = netTxLen - netTxSent;
            if (n > NET_CHUNK_SIZE)
                n = NET_CHUNK_SIZE;
            memcpy(netChunk, netTxBuf + netTxSent, n);
            netTxSent += n;
            chunkLen = n;
            if (netTxSent == netTxLen) {
                netTxSent = 0;
                netTxLen = 0;
            }
        }
        mutexUnlock(&queueMutex);

        if (chunkLen == 0)
            return true;
    }

    /* 先探一次可写再 send。Horizon 上 fcntl 设非阻塞不可靠,
       这样即使 socket 仍是阻塞模式,也不会在弱网时卡住发送线程。 */
    {
        struct pollfd po = { fd, POLLOUT, 0 };
        int pr = poll(&po, 1, 0);
        if (pr == 0)
            return true;
        if (pr < 0 && errno != EINTR)
            return false;
    }

    ssize_t n = send(fd, netChunk + chunkSent, chunkLen - chunkSent, 0);
    if (n > 0) {
        chunkSent += (size_t)n;
        if (chunkSent == chunkLen) {
            chunkLen = 0;
            chunkSent = 0;
        }
        return true;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return true;
    {
        static int errLogged = 0;
        if (!errLogged) {
            errLogged = 1;
            sdLogRc("无线 send 失败 errno", (Result)errno);
        }
    }
    return false;
}

/* 读取客户端心跳。1 = 收到数据,0 = 暂无数据,-1 = 连接已关闭。
   用 poll 先探可读,避免在 MSG_DONTWAIT/阻塞语义不确定时卡住会话线程。 */
int streamServerNetRead(int fd)
{
    char buf[64];

    struct pollfd pi = { fd, POLLIN, 0 };
    int pr = poll(&pi, 1, 0);
    if (pr == 0)
        return 0;
    if (pr < 0)
        return errno == EINTR ? 0 : -1;

    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n > 0)
        return 1;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
        return 0;
    return -1;
}

/* 调用方必须持有 queueMutex。
   返回 false 表示这个视频包被丢弃,调用方应置位重同步标志。
   音频在队列过半时直接丢弃:拥塞时宁可静音,也不让它拖住视频。 */
static bool netEnqueueLocked(const void* data, size_t length, bool isVideo)
{
    if (length == 0 || length > NET_TX_BUF_SIZE)
        return false;

    if (!isVideo && netTxLen - netTxSent > NET_TX_BUF_SIZE / 2)
        return true;

    if (netTxSent == netTxLen) {
        netTxSent = 0;
        netTxLen = 0;
    } else if (netTxSent > 0 && NET_TX_BUF_SIZE - netTxLen < length) {
        /* 回收已发送部分,腾出尾部空间 */
        memmove(netTxBuf, netTxBuf + netTxSent, netTxLen - netTxSent);
        netTxLen -= netTxSent;
        netTxSent = 0;
    }

    if (NET_TX_BUF_SIZE - netTxLen < length) {
        if (netDropped++ == 0)
            sdLog("无线发送队列满, 开始丢包");
        return false;
    }

    memcpy(netTxBuf + netTxLen, data, length);
    netTxLen += length;
    return true;
}

/* 视频/音频线程共用的发送入口。
   无线会话走独立的 queueMutex,USB 卡死不会拖住无线通道。 */
/* USB 队列:入队不阻塞,队满丢包 */
static bool usbEnqueue(const void* data, size_t length)
{
    bool ok = false;

    mutexLock(&usbQueueMutex);
    if (usbTxLen == usbTxSent) {
        usbTxLen = 0;
        usbTxSent = 0;
    }
    if (USB_TX_BUF_SIZE - usbTxLen >= length) {
        memcpy(usbTxBuf + usbTxLen, data, length);
        usbTxLen += length;
        ok = true;
    }
    mutexUnlock(&usbQueueMutex);

    return ok;
}

static size_t usbTakeChunk(void)
{
    size_t n = 0;

    mutexLock(&usbQueueMutex);
    if (usbTxLen > usbTxSent) {
        n = usbTxLen - usbTxSent;
        if (n > sizeof(usbChunk))
            n = sizeof(usbChunk);
        memcpy(usbChunk, usbTxBuf + usbTxSent, n);
        usbTxSent += n;
        if (usbTxSent == usbTxLen)
            usbTxSent = usbTxLen = 0;
    }
    mutexUnlock(&usbQueueMutex);

    return n;
}

static void usbResetQueue(void)
{
    mutexLock(&usbQueueMutex);
    usbTxSent = usbTxLen = 0;
    mutexUnlock(&usbQueueMutex);
}

static bool sendPacket(const void* data, size_t length)
{
    const SwlinkStreamHeader* hdr = (const SwlinkStreamHeader*)data;
    const bool isVideo = hdr->type == SwlinkStream_Video;

    if (netFdPub >= 0) {
        bool success;

        mutexLock(&queueMutex);
        success = netFd >= 0 ? netEnqueueLocked(data, length, isVideo) : false;
        mutexUnlock(&queueMutex);
        return success;
    }

    if (videoConnected || audioConnected) {
        /* USB 队列满同样返回失败,视频线程据此从关键帧重新对齐 */
        return usbEnqueue(data, length);
    }

    return false;
}

static void disconnectClient(void)
{
    videoConnected = false;
    audioConnected = false;
    usbResetQueue();
}

/* 握手:循环发 HELLO 直到主机读到(主机可能刚插线还没开始读),
   然后只读一次请求。重复发 HELLO 会污染响应流,这里只发成功一次。 */
static bool handshake(void)
{
    SwlinkHandshakeRequest req;

    size_t num = 0;
    u64 nextLogTick = 0;
    do {
        /* 主机拔线后 usb:ds 端点会失效,检测到掉线就重建,否则再插线也收不到 HELLO */
        if (swlinkUsbRecoverIfLost())
            continue;
        num = swlinkUsbWrite(USB_INTF_STREAM, SWLINK_HELLO, sizeof(SWLINK_HELLO), HANDSHAKE_TIMEOUT);
        if (num == 0) {
            u64 now = armGetSystemTick();
            if (nextLogTick == 0 || now >= nextLogTick) {
                nextLogTick = now + armNsToTicks(2000000000ULL);
                sdLogRc("HELLO 发送失败", swlinkUsbLastError());
            }
        }
    } while (num == 0 && g_streamRunning);

    if (num != sizeof(SWLINK_HELLO) || !g_streamRunning)
        return false;

    size_t got = swlinkUsbRead(USB_INTF_STREAM, &req, sizeof(req), HANDSHAKE_TIMEOUT);
    if (got != sizeof(req)) {
        sdLogRc("握手读请求字节数", (Result)got);
        return false;
    }

    SwlinkHandshakeResponse resp = { 0 };

    if (req.magic != SWLINK_HANDSHAKE_MAGIC)
        resp.code = SWLINK_HANDSHAKE_BAD_MAGIC;
    else if (req.proto_ver != ('0' | ('1' << 8)))
        resp.code = SWLINK_HANDSHAKE_BAD_VERSION;
    else
        resp.code = SWLINK_HANDSHAKE_OK;

    if (swlinkUsbWrite(USB_INTF_STREAM, &resp, sizeof(resp), HANDSHAKE_TIMEOUT) != sizeof(resp)) {
        sdLogRc("握手响应发送失败", swlinkUsbLastError());
        return false;
    }

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

/* USB 发送线程:唯一触碰 USB 流的线程。
   采集线程只入队,因此 USB 主机不读导致 usbComms 卡死时,
   只会卡住这个线程,不会影响视频采集和无线通道。 */
void streamUsbThread(void* arg)
{
    (void)arg;

    sdLog("USB 发送线程已进入");

    while (g_streamRunning) {
        if (!videoConnected && !audioConnected) {
            usbResetQueue();
            (void)handshake();
            if (!videoConnected && !audioConnected)
                svcSleepThread(2E+8L);
            continue;
        }

        size_t n = usbTakeChunk();
        if (n == 0) {
            svcSleepThread(1E+6L);
            continue;
        }

        size_t written = swlinkUsbWrite(USB_INTF_STREAM, usbChunk, n, SEND_TIMEOUT_USB);
        if (written != n) {
            logWriteFail(written, n);
            disconnectClient();
            /* 确认是主机断开(而不是主机暂时不读)就立刻重建 usb:ds,
               这样短时间内重新插线也能再次握手 */
            swlinkUsbNoteLinkLost();
        }
    }
}

void streamVideoThread(void* arg)
{
    (void)arg;

    while (g_streamRunning) {
        if (!clientConnected()) {
            svcSleepThread(5E+7L);
            continue;
        }

        if (!videoWanted()) {
            svcSleepThread(1E+8L);
            continue;
        }

        /* 丢过包就从关键帧重新对齐:一直要求补 SPS/PPS,
           并丢掉非 IDR 帧,避免解码器停在花屏/黑屏状态 */
        const bool waitingKey = videoResync;
        if (waitingKey)
            captureVideoConnected();   /* 让下一帧带上 SPS/PPS */

        /* grc 失败也会产出错误包,照常发送 */
        captureReadVideo();

        if (!g_streamRunning)
            continue;

        if (waitingKey) {
            if (!captureVideoIsKeyframe())
                continue;              /* 等下一个 IDR */
            videoResync = false;
        }

        /* 发送只入队,队满丢帧;丢帧后从下一个关键帧重新开始 */
        if (!sendPacket(&VPkt, VPkt.header.data_size + sizeof(SwlinkStreamHeader)))
            videoResync = true;
    }
}

void streamAudioThread(void* arg)
{
    (void)arg;

    while (g_streamRunning) {
        if (audioWanted()) {
            captureReadAudio();

            if (!g_streamRunning)
                continue;

            sendPacket(&APkt, APkt.header.data_size + sizeof(SwlinkStreamHeader));
        } else {
            svcSleepThread(1E+8L);
        }
    }
}
