#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

/* nxlink 主机侧实现(与 devkitPro nxlink / nx-hbmenu netloader 协议一致)。

   两个方向都用 28280 端口:
   - 上传:本机作为客户端连到 Switch 的 28280（hbmenu 的 netloader 在监听）,
     依次发送 文件名长度 / 文件名 / 文件长度,再发送 zlib 压缩后的分块数据,
     最后发命令行长度。
   - stdout 转发:被拉起的程序通过 nxlinkStdio() 主动连回本机 28280,
     所以上传前就要在本地开始监听。 */
class NxlinkHost {
public:
    ~NxlinkHost();

    /* 在本地 port 监听,接收程序的 stdout/stderr */
    bool startStdio(int port, std::string* err);
    void stopStdio();
    bool stdioListening() const { return m_listening.load(); }
    bool stdioConnected() const { return m_connected.load(); }

    /* 把 nroPath 上传到 host:port,在 Switch 上保存为 sdPath(需以 .nro 结尾) */
    bool upload(const std::string& host, int port,
                const std::string& nroPath, const std::string& sdPath,
                std::string* err);

    std::string logSnapshot();
    void clearLog();

private:
    void stdioThread();
    void appendLog(const std::string& s);

    std::atomic<bool> m_listening{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_stop{false};

    int m_listenFd = -1;
    std::thread m_thread;

    std::mutex m_logMutex;
    std::string m_log;
};
