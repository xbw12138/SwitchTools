/*
 * cli.cpp
 * SwLink 命令行工具:通过 USB 操作 Switch SD 卡(列目录/上传/下载/NRO 推送)
 *
 * 用法见 usage()。路径以 / 开头表示 SD 卡根;本地目录上传/远端目录下载均递归。
 */
#include "usb_link.hpp"
#include "file_client.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

static void usage()
{
    printf(
        "用法: swlink-cli <命令> [参数]\n"
        "\n"
        "  ls    [路径]                 列出 SD 卡目录(默认 /)\n"
        "  get   <远端> <本地>          下载文件;远端为目录时递归下载到本地目录\n"
        "  put   <本地> [远端目录或路径] 上传文件或目录(递归);目标是已存在目录则拼文件名,否则作为完整路径\n"
        "  nro   <本地.nro> [远端路径]  写入 NRO,默认 switch/<文件名>\n"
        "  mkdir <远端目录>\n"
        "  rm    <远端> [--dir]         删除文件或目录(递归)\n"
        "  mv    <远端旧> <远端新>\n"
        "  df                            显示 SD 卡容量\n"
        "\n"
        "示例:\n"
        "  swlink-cli ls /switch\n"
        "  swlink-cli nro ./hbmenu.nro\n"
        "  swlink-cli put ./payload /switch\n"
        "  swlink-cli get /switch/logs ./logs\n");
}

/* 统一成设备端要求的格式:以 / 开头,根目录为 "/" */
static std::string normRemote(const std::string& p)
{
    if (p.empty()) return "/";
    std::string s = p;
    if (s[0] != '/') s.insert(s.begin(), '/');
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

static std::string joinRemote(const std::string& dir, const std::string& name)
{
    if (dir == "/" || dir.empty()) return "/" + name;
    return dir + "/" + name;
}

static std::string baseName(const std::string& p)
{
    size_t pos = p.find_last_of('/');
    std::string s = (pos == std::string::npos) ? p : p.substr(pos + 1);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

static std::string dirName(const std::string& p)
{
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return p.substr(0, pos);
}

static std::string joinLocal(const std::string& dir, const std::string& name)
{
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

static bool localIsDir(const std::string& path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/* 递归创建本地目录(类似 mkdir -p) */
static bool makeLocalDirs(const std::string& path)
{
    if (path.empty() || path == "/") return true;
    if (localIsDir(path)) return true;

    const std::string parent = dirName(path);
    if (parent != path && !makeLocalDirs(parent)) return false;
    return mkdir(path.c_str(), 0755) == 0 || localIsDir(path);
}

static void progress(const char* what, uint64_t done, uint64_t total)
{
    if (total == 0) return;
    fprintf(stderr, "\r%s %llu/%llu (%d%%)   ", what,
            (unsigned long long)done, (unsigned long long)total,
            (int)(done * 100 / total));
    if (done >= total) fputc('\n', stderr);
    fflush(stderr);
}

/* 远端建目录:已存在则视为成功 */
static bool makeRemoteDir(FileClient& files, const std::string& remote)
{
    uint8_t type = 0;
    uint64_t size = 0;
    if (files.stat(remote, type, size) && type == 1)
        return true;
    return files.mkdir(remote);
}

/* 上传单个文件 */
static bool uploadFile(FileClient& files, const std::string& local, const std::string& remote)
{
    const std::string name = baseName(local);
    return files.writeFile(local, remote, [&](uint64_t done, uint64_t total) {
        progress(name.c_str(), done, total);
    });
}

/* 递归上传:目录先建目录,再逐项处理 */
static bool uploadPath(FileClient& files, const std::string& local, const std::string& remote)
{
    if (!localIsDir(local)) {
        if (!uploadFile(files, local, remote)) {
            fprintf(stderr, "上传失败 %s: %s\n", local.c_str(), files.lastError().c_str());
            return false;
        }
        return true;
    }

    if (!makeRemoteDir(files, remote)) {
        fprintf(stderr, "创建目录失败 %s: %s\n", remote.c_str(), files.lastError().c_str());
        return false;
    }

    DIR* d = opendir(local.c_str());
    if (!d) {
        fprintf(stderr, "无法读取本地目录 %s\n", local.c_str());
        return false;
    }

    bool ok = true;
    while (struct dirent* ent = readdir(d)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        const std::string l = joinLocal(local, ent->d_name);
        const std::string r = joinRemote(remote, ent->d_name);
        if (!uploadPath(files, l, r)) { ok = false; break; }
    }
    closedir(d);
    return ok;
}

/* 递归下载 */
static bool downloadPath(FileClient& files, const std::string& remote, const std::string& local)
{
    uint8_t type = 0;
    uint64_t size = 0;
    if (!files.stat(remote, type, size)) {
        fprintf(stderr, "读取 %s 失败: %s\n", remote.c_str(), files.lastError().c_str());
        return false;
    }

    if (type == 2) {
        /* 本地参数若已是目录,则把远端文件名拼进去 */
        std::string target = local;
        struct stat st;
        if (stat(local.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
            target = joinLocal(local, baseName(remote));
        if (!makeLocalDirs(dirName(target))) {
            fprintf(stderr, "无法创建本地目录 %s\n", dirName(target).c_str());
            return false;
        }
        const std::string name = baseName(remote);
        if (!files.readFile(remote, target, [&](uint64_t done, uint64_t total) {
                progress(name.c_str(), done, total);
            })) {
            fprintf(stderr, "下载失败 %s: %s\n", remote.c_str(), files.lastError().c_str());
            return false;
        }
        return true;
    }

    if (!makeLocalDirs(local)) {
        fprintf(stderr, "无法创建本地目录 %s\n", local.c_str());
        return false;
    }

    std::vector<FtEntry> entries;
    if (!files.list(remote, entries)) {
        fprintf(stderr, "列目录失败 %s: %s\n", remote.c_str(), files.lastError().c_str());
        return false;
    }

    bool ok = true;
    for (const FtEntry& e : entries) {
        const std::string r = joinRemote(remote, e.name);
        const std::string l = joinLocal(local, e.name);
        if (!downloadPath(files, r, l)) { ok = false; break; }
    }
    return ok;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const std::string cmd = argv[1];
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        usage();
        return 0;
    }

    UsbLink link;
    if (!link.open()) {
        fprintf(stderr, "连接 Switch 失败: %s\n", link.lastError());
        return 1;
    }
    /*
     * 不握手:文件接口由设备端独立线程服务,与串流会话无关。
     * 握手反而会把设备置为"已连接",客户端退出后设备无从感知(没有视频可写),
     * 下次就再也收不到 HELLO,只能重启主机。
     */

    FileClient files(link);
    bool ok = true;

    if (cmd == "ls") {
        const std::string remote = normRemote(argc > 2 ? argv[2] : "/");
        std::vector<FtEntry> entries;
        if (!files.list(remote, entries)) {
            fprintf(stderr, "列目录失败: %s\n", files.lastError().c_str());
            ok = false;
        } else {
            printf("%s:\n", remote.c_str());
            for (const FtEntry& e : entries) {
                if (e.type == 1)
                    printf("  [目录] %s\n", e.name.c_str());
                else
                    printf("  %12llu  %s\n", (unsigned long long)e.size, e.name.c_str());
            }
            printf("共 %zu 项\n", entries.size());
        }
    } else if (cmd == "get") {
        if (argc < 4) { usage(); ok = false; }
        else ok = downloadPath(files, normRemote(argv[2]), argv[3]);
    } else if (cmd == "put") {
        if (argc < 3) { usage(); ok = false; }
        else {
            const std::string local = argv[2];
            std::string remote;
            if (argc > 3) {
                const std::string arg = normRemote(argv[3]);
                uint8_t t = 0;
                uint64_t sz = 0;
                /* 目标若已是目录则拼文件名,否则视为完整目标路径 */
                remote = (files.stat(arg, t, sz) && t == 1)
                    ? joinRemote(arg, baseName(local)) : arg;
            } else {
                remote = joinRemote("/", baseName(local));
            }
            ok = uploadPath(files, local, remote);
        }
    } else if (cmd == "nro") {
        if (argc < 3) { usage(); ok = false; }
        else {
            const std::string local = argv[2];
            const std::string remote = normRemote(argc > 3 ? argv[3] : "switch/" + baseName(local));
            if (!makeRemoteDir(files, "/switch")) ok = false;
            else {
                printf("推送 %s -> %s\n", local.c_str(), remote.c_str());
                ok = uploadFile(files, local, remote);
                if (ok) printf("完成,在 Switch 的 hbmenu 中即可看到它\n");
            }
        }
    } else if (cmd == "mkdir") {
        if (argc < 3) { usage(); ok = false; }
        else if (!files.mkdir(normRemote(argv[2]))) {
            fprintf(stderr, "创建目录失败: %s\n", files.lastError().c_str());
            ok = false;
        }
    } else if (cmd == "rm") {
        if (argc < 3) { usage(); ok = false; }
        else {
            const bool dir = argc > 3 && strcmp(argv[3], "--dir") == 0;
            if (!files.remove(normRemote(argv[2]), dir)) {
                fprintf(stderr, "删除失败: %s\n", files.lastError().c_str());
                ok = false;
            }
        }
    } else if (cmd == "mv") {
        if (argc < 4) { usage(); ok = false; }
        else if (!files.rename(normRemote(argv[2]), normRemote(argv[3]))) {
            fprintf(stderr, "重命名失败: %s\n", files.lastError().c_str());
            ok = false;
        }
    } else if (cmd == "df") {
        uint64_t total = 0, freeSpace = 0;
        if (!files.statFs("/", total, freeSpace)) {
            fprintf(stderr, "读取容量失败: %s\n", files.lastError().c_str());
            ok = false;
        } else {
            printf("总容量 %.2f GB,可用 %.2f GB\n",
                   (double)total / (1024.0 * 1024.0 * 1024.0),
                   (double)freeSpace / (1024.0 * 1024.0 * 1024.0));
        }
    } else {
        fprintf(stderr, "未知命令: %s\n\n", cmd.c_str());
        usage();
        ok = false;
    }

    link.close();
    return ok ? 0 : 1;
}
