#include "nxlink_client.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

#include <zlib.h>

/* 单个压缩分块上限:与 hbmenu 的 ZLIB_CHUNK 一致 */
static const int kChunk = 16 * 1024;
/* 日志上限,避免长时间运行吃内存 */
static const size_t kLogMax = 256 * 1024;

static void setIoTimeout(int fd, int ms)
{
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* 带超时的连接 */
static int connectTimeout(const std::string& host, int port, int timeoutMs)
{
    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }

    int rc = (int)connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc != 0) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, timeoutMs) <= 0) {
            ::close(fd);
            return -1;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
            ::close(fd);
            return -1;
        }
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) & ~O_NONBLOCK);
    setIoTimeout(fd, 5000);
    return fd;
}

static bool sendAll(int fd, const void* data, size_t size)
{
    const char* p = (const char*)data;
    while (size > 0) {
        ssize_t n = send(fd, p, size, 0);
        if (n <= 0)
            return false;
        p += n;
        size -= (size_t)n;
    }
    return true;
}

static bool sendU32(int fd, uint32_t v)
{
    return sendAll(fd, &v, sizeof(v));
}

static bool recvAll(int fd, void* data, size_t size)
{
    char* p = (char*)data;
    while (size > 0) {
        ssize_t n = recv(fd, p, size, 0);
        if (n <= 0)
            return false;
        p += n;
        size -= (size_t)n;
    }
    return true;
}

static bool recvU32(int fd, uint32_t* v)
{
    return recvAll(fd, v, sizeof(*v));
}

NxlinkHost::~NxlinkHost()
{
    stopStdio();
}

void NxlinkHost::appendLog(const std::string& s)
{
    std::lock_guard<std::mutex> lk(m_logMutex);
    m_log += s;
    if (m_log.size() > kLogMax)
        m_log.erase(0, m_log.size() - kLogMax);
}

bool NxlinkHost::startStdio(int port, std::string* err)
{
    if (m_listening.load())
        return true;

    int fd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        if (err) *err = "创建 socket 失败";
        return false;
    }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(fd, 4) != 0) {
        if (err) *err = "端口 " + std::to_string(port) + " 绑定失败(可能被占用)";
        ::close(fd);
        return false;
    }

    m_listenFd = fd;
    m_stop = false;
    m_listening = true;
    m_thread = std::thread(&NxlinkHost::stdioThread, this);
    return true;
}

void NxlinkHost::stopStdio()
{
    if (!m_listening.load())
        return;

    m_stop = true;
    if (m_thread.joinable())
        m_thread.join();

    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        m_listenFd = -1;
    }
    m_connected = false;
    m_listening = false;
}

void NxlinkHost::stdioThread()
{
    int listenFd = m_listenFd;

    while (!m_stop.load()) {
        struct pollfd pfd;
        pfd.fd = listenFd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 200) <= 0)
            continue;

        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        int fd = (int)accept(listenFd, (struct sockaddr*)&from, &fromLen);
        if (fd < 0)
            continue;

        setIoTimeout(fd, 60000);
        m_connected = true;
        appendLog("\n[nxlink] 程序已连接,开始接收输出\n");

        char buf[4096];
        while (!m_stop.load()) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
                break;
            appendLog(std::string(buf, (size_t)n));
        }

        ::close(fd);
        m_connected = false;
        appendLog("\n[nxlink] 连接已结束\n");
    }
}

bool NxlinkHost::upload(const std::string& host, int port,
                        const std::string& nroPath, const std::string& sdPath,
                        std::string* err)
{
    FILE* f = fopen(nroPath.c_str(), "rb");
    if (!f) {
        if (err) *err = "无法打开本地文件: " + nroPath;
        return false;
    }

    struct stat st;
    if (fstat(fileno(f), &st) != 0 || st.st_size <= 0) {
        fclose(f);
        if (err) *err = "文件为空或无法读取大小";
        return false;
    }
    const uint32_t fileLen = (uint32_t)st.st_size;

    int fd = connectTimeout(host, port, 4000);
    if (fd < 0) {
        fclose(f);
        if (err)
            *err = "连接 " + host + ":" + std::to_string(port) +
                   " 失败(请先在 Switch 上打开 hbmenu)";
        return false;
    }

    bool ok = true;
    std::string why;

    /* 1. 文件名长度 + 文件名(SD 上的相对路径) */
    const uint32_t nameLen = (uint32_t)sdPath.size();
    if (!sendU32(fd, nameLen) ||
        !sendAll(fd, sdPath.data(), sdPath.size())) {
        ok = false;
        why = "发送文件名失败";
    }

    uint32_t response = 0;
    if (ok && !sendU32(fd, fileLen)) {
        ok = false;
        why = "发送文件长度失败";
    }
    if (ok && (!recvU32(fd, &response)) ) {
        ok = false;
        why = "未收到设备响应";
    } else if (ok && response != 0) {
        ok = false;
        why = "设备拒绝接收 (code " + std::to_string((int32_t)response) + ")";
    }

    /* 2. zlib 压缩后按 16KB 分块发送 */
    if (ok) {
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        if (deflateInit(&strm, Z_DEFAULT_COMPRESSION) != Z_OK) {
            ok = false;
            why = "初始化压缩失败";
        } else {
            unsigned char inBuf[kChunk];
            unsigned char outBuf[kChunk];
            uint32_t total = 0;

            while (ok) {
                size_t got = fread(inBuf, 1, sizeof(inBuf), f);
                if (got == 0)
                    break;

                strm.avail_in = (uInt)got;
                strm.next_in = inBuf;

                do {
                    strm.avail_out = kChunk;
                    strm.next_out = outBuf;
                    deflate(&strm, Z_NO_FLUSH);
                    uint32_t produced = (uint32_t)(kChunk - strm.avail_out);

                    if (produced > 0) {
                        if (!sendU32(fd, produced) ||
                            !sendAll(fd, outBuf, produced)) {
                            ok = false;
                            why = "发送数据块失败";
                            break;
                        }
                        total += produced;
                    }
                } while (strm.avail_out == 0);
            }

            /* 收尾:把剩余压缩数据冲出去 */
            if (ok) {
                int zret = Z_OK;
                while (zret != Z_STREAM_END) {
                    strm.avail_out = kChunk;
                    strm.next_out = outBuf;
                    zret = deflate(&strm, Z_FINISH);
                    uint32_t produced = (uint32_t)(kChunk - strm.avail_out);
                    if (produced > 0) {
                        if (!sendU32(fd, produced) ||
                            !sendAll(fd, outBuf, produced)) {
                            ok = false;
                            why = "发送结尾数据失败";
                            break;
                        }
                        total += produced;
                    }
                    if (zret == Z_STREAM_ERROR) {
                        ok = false;
                        why = "压缩失败";
                        break;
                    }
                }
            }

            deflateEnd(&strm);

            /* 3. 等待写入完成确认,再发送命令行长度(0 表示无附加参数) */
            if (ok && (!recvU32(fd, &response) || response != 0)) {
                ok = false;
                why = "设备报告写入失败";
            }
            if (ok && !sendU32(fd, 0)) {
                ok = false;
                why = "发送命令行失败";
            }
        }
    }

    /* 让 hbmenu 读到 EOF 再收尾 */
    shutdown(fd, SHUT_WR);
    ::close(fd);
    fclose(f);

    if (!ok) {
        if (err) *err = why;
        return false;
    }
    return true;
}

std::string NxlinkHost::logSnapshot()
{
    std::lock_guard<std::mutex> lk(m_logMutex);
    return m_log;
}

void NxlinkHost::clearLog()
{
    std::lock_guard<std::mutex> lk(m_logMutex);
    m_log.clear();
}
