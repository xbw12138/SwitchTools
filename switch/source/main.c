/*
 * main.c
 * SwLink sysmodule 入口:无堆、全静态分配
 * 线程模型: main = 视频线程;另有音频线程、文件服务线程
 */
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "usb_comms.h"
#include "capture.h"
#include "stream_server.h"
#include "file_server.h"

/* ---- 禁用堆:覆盖 libnx 符号,确保链接器不带入 malloc ---- */
void* __libnx_aligned_alloc(size_t alignment, size_t size)
{
    (void)alignment; (void)size;
    fatalThrow(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
    return NULL;
}

void __libnx_free(void* p)
{
    (void)p;
    fatalThrow(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
}

void __libnx_initheap(void)
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = NULL;
    fake_heap_end = NULL;
}

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

volatile bool g_streamRunning = true;
volatile bool g_fileRunning = true;

static Result fsInitResult;
static FsFileSystem sdCard;

void __attribute__((weak)) __appInit(void)
{
    svcSleepThread(20E+9); /* 等系统服务就绪 */

    Result rc = smInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = fsInitialize();
    if (R_FAILED(rc))
        fatalThrow(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }
    if (R_FAILED(rc))
        fatalThrow(rc);

    /* 失败也无所谓,文件服务会在使用时报错 */
    fsInitResult = fsOpenSdCardFileSystem(&sdCard);
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

    rc = captureInitialize();
    if (R_FAILED(rc))
        fatalThrow(rc);

    rc = usbCommsInitialize();
    if (R_FAILED(rc))
        fatalThrow(rc);

    if (R_SUCCEEDED(fsInitResult))
        fileServerInit(&sdCard);

    static alignas(0x1000) u8 audioStack[0x2000];
    static alignas(0x1000) u8 fileStack[0x2000];
    Thread audioThread, fileThread;

    rc = threadCreate(&audioThread, streamAudioThread, NULL, audioStack, sizeof(audioStack), 0x2C, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&audioThread);
    if (R_FAILED(rc)) fatalThrow(rc);

    rc = threadCreate(&fileThread, fileServerThread, NULL, fileStack, sizeof(fileStack), 0x2C, 3);
    if (R_SUCCEEDED(rc)) rc = threadStart(&fileThread);
    if (R_FAILED(rc)) fatalThrow(rc);

    /* 视频采集/发送跑在 main 线程上 */
    streamVideoThread(NULL);

    g_streamRunning = false;
    g_fileRunning = false;

    threadWaitForExit(&audioThread);
    threadClose(&audioThread);
    threadWaitForExit(&fileThread);
    threadClose(&fileThread);

    usbCommsExit();
    captureFinalize();

    return 0;
}
