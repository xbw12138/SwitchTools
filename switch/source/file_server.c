/*
 * file_server.c
 * SD 卡文件传输服务实现 (原生 fs IPC,不经过 fsdev,无堆分配)
 */
#include <string.h>
#include "file_server.h"
#include "usb_link.h"
#include "../../protocol/swlink_protocol.h"

#define USB_TIMEOUT 3E+9

/* 头 + 最大 payload 的静态收发缓冲,单次写回保证原子性 */
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

static size_t respond(SwlinkFtHeader* resp, const void* payload, u32 payload_len)
{
    size_t total = sizeof(SwlinkFtHeader) + payload_len;
    resp->payload_len = payload_len;
    memcpy(g_buf, resp, sizeof(SwlinkFtHeader));
    /* 处理函数可能已直接在 g_buf payload 区构建数据,避免同地址拷贝 */
    if (payload_len && (const u8*)payload != g_buf + sizeof(SwlinkFtHeader))
        memcpy(g_buf + sizeof(SwlinkFtHeader), payload, payload_len);
    return swlinkUsbWrite(USB_INTF_FILE, g_buf, total, USB_TIMEOUT) == total ? total : 0;
}

/* LIST: 请求 offset 为起始条目索引;响应 offset 为下一索引(0 表示读完) */
static u32 handleList(SwlinkFtHeader* resp, const char* path, u64 startIndex)
{
    FsDir dir;
    Result rc = fsFsOpenDirectory(&g_sd, path, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &dir);
    if (R_FAILED(rc)) return mapError(rc);

    u8* out = g_buf + sizeof(SwlinkFtHeader);
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

    return respond(resp, out, (u32)(w - out)) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleStat(SwlinkFtHeader* resp, const char* path)
{
    FsDirEntryType type;
    Result rc = fsFsGetEntryType(&g_sd, path, &type);
    if (R_FAILED(rc)) return mapError(rc);

    struct { u8 type; u64 size; } st;
    st.type = (type == FsDirEntryType_Dir) ? 1 : 2;
    st.size = 0;

    if (st.type == 2) {
        FsFile f;
        rc = fsFsOpenFile(&g_sd, path, FsOpenMode_Read, &f);
        if (R_FAILED(rc)) return mapError(rc);
        s64 sz = 0;
        fsFileGetSize(&f, &sz);
        fsFileClose(&f);
        st.size = (u64)sz;
    }

    memcpy(g_buf + sizeof(SwlinkFtHeader), &st, sizeof(st));
    return respond(resp, &st, sizeof(st)) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleRead(SwlinkFtHeader* resp, const char* path, u64 offset, u64 size)
{
    if (size > SWLINK_FT_CHUNK) size = SWLINK_FT_CHUNK;

    FsFile f;
    Result rc = fsFsOpenFile(&g_sd, path, FsOpenMode_Read, &f);
    if (R_FAILED(rc)) return mapError(rc);

    u8* out = g_buf + sizeof(SwlinkFtHeader);
    u64 done = 0;
    rc = fsFileRead(&f, offset, out, size, FsReadOption_None, &done);
    fsFileClose(&f);

    if (R_FAILED(rc)) return mapError(rc);

    resp->size = done;
    return respond(resp, out, (u32)done) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

/* WRITE: 每块 open(不存在则创建)+ SetSize(offset+len) + write + flush + close */
static u32 handleWrite(SwlinkFtHeader* resp, const char* path, u64 offset, u64 size, const u8* data)
{
    if (size > SWLINK_FT_CHUNK) return SWLINK_FT_ERR_BAD_REQUEST;

    FsFile f;
    Result rc = fsFsOpenFile(&g_sd, path, FsOpenMode_Write | FsOpenMode_Append, &f);
    if (R_FAILED(rc)) {
        rc = fsFsCreateFile(&g_sd, path, offset + size, 0);
        if (R_FAILED(rc)) return mapError(rc);
        rc = fsFsOpenFile(&g_sd, path, FsOpenMode_Write, &f);
        if (R_FAILED(rc)) return mapError(rc);
    }

    rc = fsFileSetSize(&f, offset + size);
    if (R_SUCCEEDED(rc)) rc = fsFileWrite(&f, offset, data, size, FsWriteOption_Flush);
    if (R_SUCCEEDED(rc)) rc = fsFileFlush(&f);
    fsFileClose(&f);

    if (R_FAILED(rc)) return mapError(rc);
    resp->size = size;
    return respond(resp, NULL, 0) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleStatFs(SwlinkFtHeader* resp, const char* path)
{
    s64 total = 0, freeSpace = 0;
    Result rc = fsFsGetTotalSpace(&g_sd, path, &total);
    if (R_FAILED(rc)) return mapError(rc);

    rc = fsFsGetFreeSpace(&g_sd, path, &freeSpace);
    if (R_FAILED(rc)) return mapError(rc);

    struct { u64 total; u64 free; } st = { (u64)total, (u64)freeSpace };
    return respond(resp, &st, sizeof(st)) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleMkdir(SwlinkFtHeader* resp, const char* path)
{
    Result rc = fsFsCreateDirectory(&g_sd, path);
    /* 已存在视为成功,幂等 */
    if (R_FAILED(rc) && rc != 0x402)
        return mapError(rc);
    return respond(resp, NULL, 0) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleDelete(SwlinkFtHeader* resp, const char* path, bool isDir)
{
    Result rc = isDir ? fsFsDeleteDirectoryRecursively(&g_sd, path)
                      : fsFsDeleteFile(&g_sd, path);
    if (R_FAILED(rc)) return mapError(rc);
    return respond(resp, NULL, 0) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

static u32 handleRename(SwlinkFtHeader* resp, const u8* payload, u32 payload_len)
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
    Result rc = fsFsGetEntryType(&g_sd, oldPath, &type);
    if (R_FAILED(rc)) return mapError(rc);

    rc = (type == FsDirEntryType_Dir)
        ? fsFsRenameDirectory(&g_sd, oldPath, newPath)
        : fsFsRenameFile(&g_sd, oldPath, newPath);
    if (R_FAILED(rc)) return mapError(rc);

    return respond(resp, NULL, 0) ? SWLINK_FT_OK : SWLINK_FT_ERR_IO;
}

void fileServerThread(void* arg)
{
    (void)arg;

    while (g_fileRunning) {
        SwlinkFtHeader req;
        size_t got = swlinkUsbRead(USB_INTF_FILE, &req, sizeof(req), USB_TIMEOUT);
        if (got != sizeof(req)) continue;   /* 超时重来 */

        SwlinkFtHeader resp = req;          /* magic/cmd/seq 回显 */
        resp.status = SWLINK_FT_OK;
        resp.offset = 0;
        resp.size = 0;

        if (req.magic != SWLINK_FT_MAGIC || req.payload_len > SWLINK_FT_CHUNK) {
            resp.status = SWLINK_FT_ERR_BAD_REQUEST;
            respond(&resp, NULL, 0);
            continue;
        }

        u8* payload = g_buf + sizeof(SwlinkFtHeader);
        if (req.payload_len) {
            got = swlinkUsbRead(USB_INTF_FILE, payload, req.payload_len, USB_TIMEOUT);
            if (got != req.payload_len) continue;
        }

        char path[SWLINK_FT_PATH_MAX + 1];
        u32 err = SWLINK_FT_OK;

        switch (req.cmd) {
        case SwlinkFt_Ping: {
            const char* ver = SWLINK_PROTO_VERSION;
            respond(&resp, ver, sizeof(SWLINK_PROTO_VERSION));
            break;
        }
        case SwlinkFt_List:
            err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
            if (err == SWLINK_FT_OK) err = handleList(&resp, path, req.offset);
            break;
        case SwlinkFt_Stat:
            err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
            if (err == SWLINK_FT_OK) err = handleStat(&resp, path);
            break;
        case SwlinkFt_Read:
            err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
            if (err == SWLINK_FT_OK) err = handleRead(&resp, path, req.offset, req.size);
            break;
        case SwlinkFt_Write: {
            /* payload = [u16 len][path][data...] */
            u32 consumed = 0;
            err = parsePath(payload, req.payload_len, path, sizeof(path), &consumed);
            if (err == SWLINK_FT_OK) {
                if (consumed + req.size > req.payload_len)
                    err = SWLINK_FT_ERR_BAD_REQUEST;
                else
                    err = handleWrite(&resp, path, req.offset, req.size, payload + consumed);
            }
            break;
        }
        case SwlinkFt_Mkdir:
            err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
            if (err == SWLINK_FT_OK) err = handleMkdir(&resp, path);
            break;
        case SwlinkFt_Delete:
            err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
            if (err == SWLINK_FT_OK) err = handleDelete(&resp, path, (req.offset & 1) != 0);
            break;
        case SwlinkFt_Rename:
            err = handleRename(&resp, payload, req.payload_len);
            break;
        case SwlinkFt_StatFs:
            err = parsePath(payload, req.payload_len, path, sizeof(path), NULL);
            if (err == SWLINK_FT_OK) err = handleStatFs(&resp, path);
            break;
        default:
            err = SWLINK_FT_ERR_BAD_REQUEST;
            break;
        }

        /* 处理函数内部成功时已自行回响应;这里统一回错误 */
        if (err != SWLINK_FT_OK) {
            resp.status = err;
            respond(&resp, NULL, 0);
        }
    }
}
