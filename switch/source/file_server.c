/*
 * file_server.c
 * SD 卡文件传输服务:处理 USB 文件接口上的请求
 * (原生 fs IPC,不经过 fsdev,无堆分配)
 */
#include <string.h>

#include "file_server.h"
#include "usb_link.h"
#include "../../protocol/swlink_protocol.h"

#define FT_TIMEOUT 3E+9
/* 读请求时每 0.5s 醒一次,给退出标志和断线检测留出机会 */
#define FT_SLICE   5E+8

typedef struct {
    u8* buf;              /* sizeof(SwlinkFtHeader) + SWLINK_FT_CHUNK */
    FsFileSystem* sd;
} FileSession;

static alignas(0x1000) u8 g_buf[sizeof(SwlinkFtHeader) + SWLINK_FT_CHUNK];
static FsFileSystem g_sd;

void fileServerInit(FsFileSystem* sd)
{
    memcpy(&g_sd, sd, sizeof(FsFileSystem));
}

static u32 mapError(Result rc)
{
    if (R_SUCCEEDED(rc)) return SWLINK_FT_OK;
    if (rc == 0x202) return SWLINK_FT_ERR_NOT_FOUND;
    return SWLINK_FT_ERR_IO;
}

/* 从 payload 解析 [u16 len][bytes] 路径,转换为 NUL 结尾 */
static u32 parsePath(const u8* payload, u32 payload_len, char* out, u32 out_size, u32* consumed)
{
    u16 len;
    if (payload_len < sizeof(u16)) return SWLINK_FT_ERR_BAD_REQUEST;
    memcpy(&len, payload, sizeof(u16));
    if (len == 0 || len >= out_size || sizeof(u16) + len > payload_len)
        return SWLINK_FT_ERR_BAD_REQUEST;
    if (len > SWLINK_FT_PATH_MAX) return SWLINK_FT_ERR_PATH_TOO_LONG;

    memcpy(out, payload + sizeof(u16), len);
    out[len] = '\0';

    /* 必须以 '/' 开头,限制在 SD 根内 */
    if (out[0] != '/') return SWLINK_FT_ERR_BAD_REQUEST;

    if (consumed) *consumed = sizeof(u16) + len;
    return SWLINK_FT_OK;
}

static size_t respond(FileSession* s, SwlinkFtHeader* resp, const void* payload, u32 payload_len)
{
    size_t total = sizeof(SwlinkFtHeader) + payload_len;
    resp->payload_len = payload_len;
    memcpy(s->buf, resp, sizeof(SwlinkFtHeader));
    /* 处理函数可能已直接在 buf payload 区构建数据,避免同地址拷贝 */
    if (payload_len && (const u8*)payload != s->buf + sizeof(SwlinkFtHeader))
        memcpy(s->buf + sizeof(SwlinkFtHeader), payload, payload_len);
    return swlinkUsbWrite(USB_INTF_FILE, s->buf, total, FT_TIMEOUT) == total ? total : 0;
}

/* 收满 size 字节;false = 断开或已停止运行 */
static bool readFull(FileSession* s, void* dst, u32 size)
{
    u8* p = (u8*)dst;
    u32 got = 0;
    int idleSlices = 0;

    while (got < size) {
        if (!g_fileRunning)
            return false;

        size_t n = swlinkUsbRead(USB_INTF_FILE, p + got, size - got, FT_SLICE);
        if (n == 0) {
            /* 半截请求停滞 3 秒(USB 拔插):丢弃并重新同步 */
            if (got > 0 && ++idleSlices >= 6)
                return false;
            continue;
        }
        idleSlices = 0;
        got += (u32)n;
    }
    return true;
}

/* LIST: 请求 offset 为起始条目索引;响应 offset 为下一索引(0 表示读完) */
static u32 handleList(FileSession* s, SwlinkFtHeader* resp, const char* path, u64 startIndex)
{
    FsDir dir;
    Result rc = fsFsOpenDirectory(s->sd, path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) return mapError(rc);

    u8* out = s->buf + sizeof(SwlinkFtHeader);
    u8* w = out;
    u64 index = 0;
    u32 count = 0;
    bool more = false;

    while (count < SWLINK_FT_LIST_BATCH) {
        FsDirectoryEntry ent;
        s64 total = 0;
        rc = fsDirRead(&dir, &total, 1, &ent);
        if (R_FAILED(rc) || total == 0) break;

        if (index++ < startIndex) continue;

        /* 64 条最大名字 0x300 时总长约 50KB,远小于 CHUNK,无需检查容量 */
        u16 name_len = (u16)strlen(ent.name);
        u8 type = (ent.type == FsDirEntryType_Dir) ? 1 : 2;
        u64 size = (u64)ent.file_size;

        memcpy(w, &type, 1);      w += 1;
        memcpy(w, &size, 8);      w += 8;
        memcpy(w, &name_len, 2);  w += 2;
        memcpy(w, ent.name, name_len); w += name_len;
        count++;
    }

    /* 是否还有剩余条目 */
    if (!more) {
        FsDirectoryEntry ent;
        s64 total = 0;
        if (R_SUCCEEDED(fsDirRead(&dir, &total, 1, &ent)) && total > 0)
            more = true;
    }

    fsDirClose(&dir);

    resp->offset = more ? (startIndex + count) : 0;
    resp->size = count;

    return respond(s, resp, out, (u32)(w - out)) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleStat(FileSession* s, SwlinkFtHeader* resp, const char* path)
{
    FsDirEntryType type;
    Result rc = fsFsGetEntryType(s->sd, path, &type);
    if (R_FAILED(rc)) return mapError(rc);

    struct { u8 type; u64 size; } st;
    st.type = (type == FsDirEntryType_Dir) ? 1 : 2;
    st.size = 0;

    if (st.type == 2) {
        FsFile f;
        rc = fsFsOpenFile(s->sd, path, FsOpenMode_Read, &f);
        if (R_FAILED(rc)) return mapError(rc);
        s64 sz = 0;
        fsFileGetSize(&f, &sz);
        fsFileClose(&f);
        st.size = (u64)sz;
    }

    return respond(s, resp, &st, sizeof(st)) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleRead(FileSession* s, SwlinkFtHeader* resp, const char* path, u64 offset, u64 size)
{
    if (size > SWLINK_FT_CHUNK) size = SWLINK_FT_CHUNK;

    FsFile f;
    Result rc = fsFsOpenFile(s->sd, path, FsOpenMode_Read, &f);
    if (R_FAILED(rc)) return mapError(rc);

    u8* out = s->buf + sizeof(SwlinkFtHeader);
    u64 done = 0;
    rc = fsFileRead(&f, offset, out, size, FsReadOption_None, &done);
    fsFileClose(&f);

    if (R_FAILED(rc)) return mapError(rc);

    resp->size = done;
    return respond(s, resp, out, (u32)done) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

/* WRITE: 每块 open(不存在则创建)+ SetSize(offset+len) + write + flush + close */
static u32 handleWrite(FileSession* s, SwlinkFtHeader* resp, const char* path, u64 offset, u64 size, const u8* data)
{
    if (size > SWLINK_FT_CHUNK) return SWLINK_FT_ERR_BAD_REQUEST;

    FsFile f;
    Result rc = fsFsOpenFile(s->sd, path, FsOpenMode_Write | FsOpenMode_Append, &f);
    if (R_FAILED(rc)) {
        rc = fsFsCreateFile(s->sd, path, offset + size, 0);
        if (R_FAILED(rc)) return mapError(rc);
        rc = fsFsOpenFile(s->sd, path, FsOpenMode_Write, &f);
        if (R_FAILED(rc)) return mapError(rc);
    }

    rc = fsFileSetSize(&f, offset + size);
    if (R_SUCCEEDED(rc)) rc = fsFileWrite(&f, offset, data, size, FsWriteOption_Flush);
    if (R_SUCCEEDED(rc)) rc = fsFileFlush(&f);
    fsFileClose(&f);

    if (R_FAILED(rc)) return mapError(rc);
    resp->size = size;
    return respond(s, resp, NULL, 0) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleStatFs(FileSession* s, SwlinkFtHeader* resp, const char* path)
{
    s64 total = 0, freeSpace = 0;
    Result rc = fsFsGetTotalSpace(s->sd, path, &total);
    if (R_FAILED(rc)) return mapError(rc);

    rc = fsFsGetFreeSpace(s->sd, path, &freeSpace);
    if (R_FAILED(rc)) return mapError(rc);

    struct { u64 total; u64 free; } st = { (u64)total, (u64)freeSpace };
    return respond(s, resp, &st, sizeof(st)) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleMkdir(FileSession* s, SwlinkFtHeader* resp, const char* path)
{
    Result rc = fsFsCreateDirectory(s->sd, path);
    /* 已存在视为成功,幂等 */
    if (R_FAILED(rc) && rc != 0x402)
        return mapError(rc);
    return respond(s, resp, NULL, 0) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleDelete(FileSession* s, SwlinkFtHeader* resp, const char* path, bool isDir)
{
    Result rc = isDir ? fsFsDeleteDirectoryRecursively(s->sd, path)
                      : fsFsDeleteFile(s->sd, path);
    if (R_FAILED(rc)) return mapError(rc);
    return respond(s, resp, NULL, 0) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleRename(FileSession* s, SwlinkFtHeader* resp, const u8* payload, u32 payload_len)
{
    char oldPath[SWLINK_FT_PATH_MAX + 1];
    char newPath[SWLINK_FT_PATH_MAX + 1];
    u32 consumed = 0;

    u32 err = parsePath(payload, payload_len, oldPath, sizeof(oldPath), &consumed);
    if (err != SWLINK_FT_OK) return err;

    if (consumed >= payload_len) return SWLINK_FT_ERR_BAD_REQUEST;
    err = parsePath(payload + consumed, payload_len - consumed, newPath, sizeof(newPath), NULL);
    if (err != SWLINK_FT_OK) return err;

    FsDirEntryType type;
    Result rc = fsFsGetEntryType(s->sd, oldPath, &type);
    if (R_FAILED(rc)) return mapError(rc);

    rc = (type == FsDirEntryType_Dir)
        ? fsFsRenameDirectory(s->sd, oldPath, newPath)
        : fsFsRenameFile(s->sd, oldPath, newPath);
    if (R_FAILED(rc)) return mapError(rc);

    return respond(s, resp, NULL, 0) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

/* 处理一次完整请求;false = 通道断开/停止 */
static bool handleOne(FileSession* s)
{
    SwlinkFtHeader req;

    if (!readFull(s, &req, sizeof(req)))
        return false;

    SwlinkFtHeader resp = req;          /* magic/cmd/seq 回显 */
    resp.status = SWLINK_FT_OK;
    resp.offset = 0;
    resp.size = 0;

    if (req.magic != SWLINK_FT_MAGIC || req.payload_len > SWLINK_FT_CHUNK) {
        resp.status = SWLINK_FT_ERR_BAD_REQUEST;
        respond(s, &resp, NULL, 0);
        return true;
    }

    u8* payload = s->buf + sizeof(SwlinkFtHeader);
    if (req.payload_len && !readFull(s, payload, req.payload_len))
        return false;

    char path[SWLINK_FT_PATH_MAX + 1];
    u32 err = SWLINK_FT_OK;

    switch (req.cmd) {
    case SwlinkFt_Ping: {
        const char* ver = SWLINK_PROTO_VERSION;
        respond(s, &resp, ver, sizeof(SWLINK_PROTO_VERSION));
        break;
    }
    case SwlinkFt_List:
        err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
        if (err == SWLINK_FT_OK) err = handleList(s, &resp, path, req.offset);
        break;
    case SwlinkFt_Stat:
        err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
        if (err == SWLINK_FT_OK) err = handleStat(s, &resp, path);
        break;
    case SwlinkFt_Read:
        err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
        if (err == SWLINK_FT_OK) err = handleRead(s, &resp, path, req.offset, req.size);
        break;
    case SwlinkFt_Write: {
        /* payload = [u16 len][path][data...] */
        u32 consumed = 0;
        err = parsePath(payload, req.payload_len, path, sizeof(path), &consumed);
        if (err == SWLINK_FT_OK) {
            if (consumed + req.size > req.payload_len)
                err = SWLINK_FT_ERR_BAD_REQUEST;
            else
                err = handleWrite(s, &resp, path, req.offset, req.size, payload + consumed);
        }
        break;
    }
    case SwlinkFt_Mkdir:
        err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
        if (err == SWLINK_FT_OK) err = handleMkdir(s, &resp, path);
        break;
    case SwlinkFt_Delete:
        err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
        if (err == SWLINK_FT_OK) err = handleDelete(s, &resp, path, (req.offset & 1) != 0);
        break;
    case SwlinkFt_Rename:
        err = handleRename(s, &resp, payload, req.payload_len);
        break;
    case SwlinkFt_StatFs:
        err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
        if (err == SWLINK_FT_OK) err = handleStatFs(s, &resp, path);
        break;
    default:
        err = SWLINK_FT_ERR_BAD_REQUEST;
        break;
    }

    /* 处理函数内部成功时已自行回响应;这里统一回错误 */
    if (err != SWLINK_FT_OK) {
        resp.status = err;
        respond(s, &resp, NULL, 0);
    }
    return true;
}

void fileServerThread(void* arg)
{
    (void)arg;

    FileSession s = { .buf = g_buf, .sd = &g_sd };

    while (g_fileRunning) {
        /* USB 超时只会返回 0,readFull 会继续等;false 仅代表半截请求作废 */
        handleOne(&s);
    }
}
