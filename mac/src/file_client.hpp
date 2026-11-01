/*
 * file_client.hpp
 * 文件传输客户端:同步请求/响应,支持分块读写与进度回调
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

class UsbLink;

struct FtEntry {
    uint8_t type;        /* 1=目录 2=文件 */
    uint64_t size;
    std::string name;
};

class FileClient {
public:
    using ProgressCb = std::function<void(uint64_t done, uint64_t total)>;

    explicit FileClient(UsbLink& link) : link_(link) {}

    bool ping();
    bool list(const std::string& path, std::vector<FtEntry>& out);
    bool stat(const std::string& path, uint8_t& type, uint64_t& size);
    bool statFs(const std::string& path, uint64_t& total, uint64_t& free);
    bool readFile(const std::string& remote, const std::string& local, ProgressCb cb = nullptr);
    bool writeFile(const std::string& local, const std::string& remote, ProgressCb cb = nullptr);
    bool mkdir(const std::string& path);
    bool remove(const std::string& path, bool isDir);
    bool rename(const std::string& oldPath, const std::string& newPath);

    /* 取消标志:置 true 后分块循环会在下一个块检查时退出 */
    void setCancelFlag(std::atomic<bool>* flag) { cancel_ = flag; }
    bool cancelled() const { return cancel_ && cancel_->load(); }

    const std::string& lastError() const { return err_; }

private:
    /* 发请求并收完整响应;成功时 respPayload 为响应负载 */
    bool request(uint32_t cmd, uint64_t offset, uint64_t size,
                 const std::vector<uint8_t>& payload,
                 std::vector<uint8_t>& respPayload,
                 uint64_t* respOffset = nullptr, uint64_t* respSize = nullptr);

    static std::vector<uint8_t> packPath(const std::string& path);

    UsbLink& link_;
    uint32_t seq_ = 0;
    std::string err_;
    std::atomic<bool>* cancel_ = nullptr;
    /* 请求-响应必须串行:传输线程与 UI 线程可能同时访问文件接口 */
    std::mutex ioMutex_;
};
