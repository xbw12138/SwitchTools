/*
 * main.c
 * SwLink sysmodule 入口:无堆、全静态分配
 * 线程模型: main = 视频线程;另有音频线程、文件服务线程
 */
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "usb_link.h"
#include "capture.h"
#include "stream_server.h"
#include "file_server.h"
#include "net_server.h"

/* ---- 静态内堆 ----
 * (无堆分配,仅 newlib/运行时的少量需求)
 */
#define INNER_HEAP_SIZE 0x200000
size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char nx_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = nx_inner_heap;
    fake_heap_end = nx_inner_heap + INNER_HEAP_SIZE;
}

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;
u32 __nx_fsdev_direntry_cache_size = 1;

volatile bool g_streamRunning = true;
volatile bool g_fileRunning = true;

static Result fsInitResult;
static FsFileSystem sdCard;

/*
 * 把运行日志追加写到 SD 卡的 /SwLink.log。
 * sysmodule 没有控制台,出问题时这是唯一的排查手段。
 * 注意: 不用 printf 系列,避免把 newlib 的 malloc 拉进链接 (本模块禁堆)。
 */
static Mutex g_logMutex;
static bool g_logMutexReady = false;

static void sdLogRaw(const void* data, size_t len)
{
    if (R_FAILED(fsInitResult) || len == 0)
        return;

    if (g_logMutexReady)
        mutexLock(&g_logMutex);

    FsFile file;
    s64 size = 0;
    if (R_FAILED(fsFsOpenFile(&sdCard, "/SwLink.log", FsOpenMode_Write | FsOpenMode_Append, &file))) {
        if (R_FAILED(fsFsCreateFile(&sdCard, "/SwLink.log", 0, 0)))
            goto out;
        if (R_FAILED(fsFsOpenFile(&sdCard, "/SwLink.log", FsOpenMode_Write | FsOpenMode_Append, &file)))
            goto out;
    }

    if (R_SUCCEEDED(fsFileGetSize(&file, &size)))
        fsFileWrite(&file, size, data, len, FsWriteOption_Flush);
    fsFileClose(&file);

out:
    if (g_logMutexReady)
        mutexUnlock(&g_logMutex);
}

/* 追加一行纯文本 (供 capture.c / stream_server.c 使用)
   一次写入整行,避免多线程各自写"正文"和"换行"导致日志错行 */
void sdLog(const char* text)
{
    char buf[256];
    size_t n = strlen(text);

    if (n > sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    memcpy(buf, text, n);
    buf[n++] = '\n';
    sdLogRaw(buf, n);
}

/* 追加 "标签: 0x12345678" */
void sdLogRc(const char* tag, Result rc)
{
    static const char hex[] = "0123456789abcdef";
    char buf[80];
    size_t n = 0;
    for (; tag[n] && n < 60; n++)
        buf[n] = tag[n];
    buf[n++] = ':';
    buf[n++] = ' ';
    buf[n++] = '0';
    buf[n++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4)
        buf[n++] = hex[(rc >> shift) & 0xF];
    buf[n++] = '\n';
    sdLogRaw(buf, n);
}

/* 服务就绪前反复重试,代替原先盲等 20 秒 */
static Result initWithRetry(Result (*fn)(void), int attempts)
{
    Result rc = fn();
    for (int i = 1; R_FAILED(rc) && i < attempts; i++) {
        svcSleepThread(500000000ULL);
        rc = fn();
    }
    return rc;
}

void __attribute__((weak)) __appInit(void)
{
    mutexInit(&g_logMutex);
    g_logMutexReady = true;

    Result rcSm = initWithRetry(smInitialize, 20);
    if (R_FAILED(rcSm))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    Result rcFs = initWithRetry(fsInitialize, 20);
    if (R_FAILED(rcFs))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    /* 尽早建立日志通道: 之后每一步失败都能在 SD 卡上看到 */
    fsInitResult = fsOpenSdCardFileSystem(&sdCard);
    sdLog("== __appInit 进入 ==");
    sdLogRc("smInitialize", rcSm);
    sdLogRc("fsInitialize", rcFs);
    sdLogRc("SD 挂载", fsInitResult);

    Result rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    sdLogRc("setsysInitialize", rc);
    if (R_FAILED(rc))
        fatalThrow(rc);

    sdLogRc("固件版本 (hex)", hosversionGet());
}

void __attribute__((weak)) __appExit(void)
{
    if (R_SUCCEEDED(fsInitResult))
        fsFsClose(&sdCard);
    fsExit();
    smExit();
}

int main(void)
{
    Result rc;

    sdLog("======== SwLink 启动 ========");
    sdLogRc("固件版本 (hex)", hosversionGet());

    /* grc:d 改为按需初始化:开机不持有采集会话,避免影响锁屏/休眠 */
    sdLog("采集服务延迟到首次客户端连接");

    /* usb:ds 在开机早期可能还没就绪,失败就重试 */
    for (int attempt = 1; attempt <= 10; attempt++) {
        rc = swlinkUsbInitialize();
        sdLogRc("usb init", rc);
        if (R_SUCCEEDED(rc))
            break;
        svcSleepThread(3000000000ULL);
    }
    if (R_FAILED(rc)) {
        sdLog("usb 初始化彻底失败, 退出");
        fatalThrow(rc);
    }
    sdLog("USB 已使能, 等待主机连接");

    if (R_SUCCEEDED(fsInitResult))
        fileServerInit(&sdCard);
    else
        sdLogRc("SD 卡挂载失败", fsInitResult);

    /* 无线接入与 USB 并存:有 Wi-Fi 客户端连上时优先走无线发送 */
    netServerInit();

    static alignas(0x1000) u8 audioStack[0x2000];
    static alignas(0x1000) u8 usbStack[0x2000];
    static alignas(0x1000) u8 fileStack[0x2000];
    Thread audioThread, usbThread, fileThread;

    streamServerInit();

    rc = threadCreate(&audioThread, streamAudioThread, NULL, audioStack, sizeof(audioStack), 0x2C, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&audioThread);
    sdLogRc("音频线程", rc);
    if (R_FAILED(rc)) fatalThrow(rc);

    rc = threadCreate(&usbThread, streamUsbThread, NULL, usbStack, sizeof(usbStack), 0x2C, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&usbThread);
    sdLogRc("USB 发送线程", rc);
    if (R_FAILED(rc)) fatalThrow(rc);

    rc = threadCreate(&fileThread, fileServerThread, NULL, fileStack, sizeof(fileStack), 0x2C, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&fileThread);
    sdLogRc("文件线程", rc);
    if (R_FAILED(rc)) fatalThrow(rc);

    /* 视频采集/发送跑在 main 线程上 */
    streamVideoThread(NULL);

    g_streamRunning = false;
    g_fileRunning = false;

    threadWaitForExit(&audioThread);
    threadClose(&audioThread);
    threadWaitForExit(&usbThread);
    threadClose(&usbThread);
    threadWaitForExit(&fileThread);
    threadClose(&fileThread);

    netServerExit();
    swlinkUsbExit();
    captureFinalize();

    return 0;
}
