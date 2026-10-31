/*
 * file_client.hpp
 * 文件传输客户端:同步请求/响应,支持分块读写与进度回调
 */
#pragma once

#include <cstdint>
#include <functional>
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
    bool readFile(const std::string& remote, const std::string& local, ProgressCb cb = nullptr);
    bool writeFile(const std::string& local, const std::string& remote, ProgressCb cb = nullptr);
    bool mkdir(const std::string& path);
    bool remove(const std::string& path, bool isDir);
    bool rename(const std::string& oldPath, const std::string& newPath);

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
};
