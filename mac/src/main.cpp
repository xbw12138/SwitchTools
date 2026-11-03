/*
 * main.cpp
 * SwLink macOS 客户端:SDL2 + ImGui 界面,串流 / 文件 两个页面
 */
#include <atomic>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include "link.hpp"
#include "usb_link.hpp"
#include "stream_client.hpp"
#include "video_decoder.hpp"
#include "audio_output.hpp"
#include "file_client.hpp"
#include "../../protocol/swlink_protocol.h"

/* ---------------- 配色 ---------------- */
#define COL_ACCENT      IM_COL32( 82,142,255,255)
#define COL_ACCENT_SOFT IM_COL32( 82,142,255, 38)
#define COL_OK          IM_COL32( 61,205,150,255)
#define COL_ERR         IM_COL32(255,107,107,255)
#define COL_TEXT        IM_COL32(230,234,240,255)
#define COL_TEXT_DIM    IM_COL32(150,158,172,255)
#define COL_TEXT_FAINT  IM_COL32(105,112,126,255)
#define COL_TOPBAR      IM_COL32( 20, 23, 29,255)
#define COL_SIDEBAR     IM_COL32( 17, 20, 25,255)
#define COL_PANEL       IM_COL32( 26, 30, 38,255)
#define COL_VIDEO_BG    IM_COL32(  9, 10, 13,255)
#define COL_ROW_SEL     IM_COL32( 82,142,255, 30)

/* ---------------- 全局状态 ---------------- */
static UsbLink g_usbLink;
static Link* g_link = &g_usbLink;
static StreamClient* g_stream = nullptr;
static VideoDecoder g_decoder;
static AudioOutput g_audio;
static FileClient* g_files = nullptr;

static ImFont* g_fontTitle = nullptr;
static float g_pixelScale = 1.0f;

static std::atomic<bool> g_connected{false};
static std::string g_status = "未连接";

/* 错误信息可能由后台线程写入,加锁保护 */
static std::mutex g_errMutex;
static std::string g_errorText;

/* NRO 推送:本地 .nro 经 USB 文件通道写入 Switch 的 SD 卡 */
static char g_nxNroPath[512] = "";
static char g_nxSdPath[256] = "switch/";
static std::atomic<bool> g_nxBusy{false};
static std::atomic<int> g_nxPercent{0};
static std::mutex g_nxStatusMutex;
static std::string g_nxStatus;
static std::string g_nxLog;

static void setNxStatus(const std::string& s)
{
    std::lock_guard<std::mutex> lk(g_nxStatusMutex);
    g_nxStatus = s;
}

static std::string nxStatus()
{
    std::lock_guard<std::mutex> lk(g_nxStatusMutex);
    return g_nxStatus;
}

static void appendNxLog(const std::string& s)
{
    std::lock_guard<std::mutex> lk(g_nxStatusMutex);
    if (!g_nxLog.empty()) g_nxLog += '\n';
    g_nxLog += s;
}

static std::string nxLog()
{
    std::lock_guard<std::mutex> lk(g_nxStatusMutex);
    return g_nxLog;
}

static SDL_Window* g_window = nullptr;
/* 独立串流窗口(可拖到副屏/单独放大) */
static SDL_Window* g_vwin = nullptr;
static SDL_Renderer* g_vrend = nullptr;
static SDL_Texture* g_vtex = nullptr;
static uint64_t g_vframe = 0;
static int g_vtexW = 0, g_vtexH = 0;
static int g_page = 0;          /* 0 = 串流, 1 = 文件, 2 = NRO 推送, 3 = 帮助 */
static bool g_muted = false;
static float g_fps = 0.0f;

static void setError(const std::string& s)
{
    std::lock_guard<std::mutex> lk(g_errMutex);
    g_errorText = s;
}

static std::string errorText()
{
    std::lock_guard<std::mutex> lk(g_errMutex);
    return g_errorText;
}

static void clearError()
{
    std::lock_guard<std::mutex> lk(g_errMutex);
    g_errorText.clear();
}

/* ---------------- 字体与主题 ---------------- */

/* 加载中文体。ImGui 内置字体只有 ASCII,不换字体中文全是乱码 */
/* UI 用到的全部汉字:ImGui 的"常用字"范围并不覆盖所有字形(如"屏"),
   缺字会渲染成方块,因此把界面实际出现的汉字补进字形范围 */
static const char kUiCjk[] =
    "一上下不与两个中串为主乱了交产人以件任优会传位住体余作使侧便保信停先免入全共关内再写出击分切列创初删到制刷前剩功加务动助包化匹区协单卡即压原双取口只可台右同名后否含启命和响器固图在地址型塞填声处备复多大失头夹始字存安完定客容小尚局层屏屑展属工左已带帧帮常并序底度延建开归当录影径待忽态总息慎戏成或截户手才打执找把折护拖持指按换据排接提搜操支改数文断新无旧时是有未本机材束条板染查标栏样根检次正此每气没法流浏消渲游滤灯点版状率现理生用由画界留略的目直码硬确示秒称程稍空窗立端等管类系索级纸线终结绘给统编置能自致色节若菜行表装要视览角解计认让议设误请读调谨象败质跨路载输边过运进远连迟选递通速避部配采里重量钮锁错锚键长间阻除集需静面音页顶项顺顿预频题首独闭显回"
    "域网搜索中填再使没鉴权络抖丢来花睡眠";

static void loadFonts()
{
    ImGuiIO& io = ImGui::GetIO();

    static const char* const kCandidates[] = {
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
    };

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.PixelSnapH = true;

    /* 范围表要活得比字体久,存成静态 */
    static ImVector<ImWchar> ranges;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    builder.AddText(kUiCjk);
    builder.BuildRanges(&ranges);

    for (const char* path : kCandidates) {
        FILE* fp = fopen(path, "rb");
        if (!fp) continue;
        fclose(fp);
        ImFont* body = io.Fonts->AddFontFromFileTTF(path, 17.0f, &cfg, ranges.Data);
        if (body) {
            g_fontTitle = io.Fonts->AddFontFromFileTTF(path, 22.0f, &cfg, ranges.Data);
            if (!g_fontTitle) g_fontTitle = body;
            return;
        }
    }

    ImFont* def = io.Fonts->AddFontDefault();
    g_fontTitle = def;
}

static void applyTheme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    ImGui::StyleColorsDark();

    s.WindowPadding     = ImVec2(0, 0);
    s.WindowBorderSize  = 0;
    s.WindowRounding    = 0;
    s.ChildRounding     = 10;
    s.ChildBorderSize   = 1;
    s.PopupRounding     = 10;
    s.PopupBorderSize   = 1;
    s.FrameRounding     = 8;
    s.FrameBorderSize   = 0;
    s.GrabRounding      = 8;
    s.TabRounding       = 8;
    s.ScrollbarRounding = 8;
    s.ItemSpacing       = ImVec2(10, 10);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.FramePadding      = ImVec2(12, 7);
    s.ScrollbarSize     = 12;
    s.SeparatorTextBorderSize = 0;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.055f, 0.063f, 0.078f, 1.00f);
    c[ImGuiCol_ChildBg]         = ImVec4(0.075f, 0.086f, 0.106f, 1.00f);
    c[ImGuiCol_PopupBg]         = ImVec4(0.10f, 0.11f, 0.14f, 0.98f);
    c[ImGuiCol_Border]          = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    c[ImGuiCol_Text]            = ImVec4(0.90f, 0.92f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]    = ImVec4(0.44f, 0.47f, 0.53f, 1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.17f, 0.19f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.20f, 0.23f, 0.28f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.21f, 0.24f, 0.30f, 1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.25f, 0.29f, 0.36f, 1.00f);
    c[ImGuiCol_Header]          = ImVec4(0.16f, 0.20f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
    c[ImGuiCol_HeaderActive]    = ImVec4(0.20f, 0.25f, 0.34f, 1.00f);
    c[ImGuiCol_TableHeaderBg]   = ImVec4(0.10f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_TableRowBg]      = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]   = ImVec4(1.00f, 1.00f, 1.00f, 0.03f);
    c[ImGuiCol_TableBorderLight] = ImVec4(1.00f, 1.00f, 1.00f, 0.05f);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.24f, 0.27f, 0.33f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.32f, 0.36f, 0.43f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.38f, 0.43f, 0.52f, 1.00f);
    c[ImGuiCol_Separator]       = ImVec4(1.00f, 1.00f, 1.00f, 0.08f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.03f, 0.04f, 0.70f);
    c[ImGuiCol_NavHighlight]    = ImVec4(0.32f, 0.55f, 1.00f, 1.00f);
}

/* ---------------- 文件页状态 ---------------- */

static char g_path[768] = "/";
static std::vector<FtEntry> g_entries;
static int g_selected = -1;              /* 主选中行 */
static std::vector<int> g_multi;         /* 多选行(Ctrl/Cmd 或 Shift 连选) */
static int g_anchor = -1;                /* Shift 连选锚点 */
static char g_filter[128] = {0};         /* 名称过滤 */
static std::vector<int> g_view;          /* 过滤后的条目下标 */
static std::string g_ftStatus;
static std::mutex g_ftMutex;
static std::atomic<bool> g_ftBusy{false};
static std::atomic<bool> g_ftCancel{false};
static std::atomic<bool> g_refreshRequest{false};
static std::atomic<uint64_t> g_ftDone{0};     /* 已传输字节 */
static std::atomic<uint64_t> g_ftTotal{0};    /* 本次任务总字节(未统计完时为 0) */
static std::atomic<uint64_t> g_sdTotal{0};    /* SD 总容量 */
static std::atomic<uint64_t> g_sdFree{0};     /* SD 剩余容量 */
static bool g_filesLoaded = false;       /* 首次进入文件页时自动列目录 */

static void refreshList();
static bool rowSelected(int idx);
static void clearSelection();
static void handleRowClick(int idx);
static void beginDownloadSelected();
static void beginUploadPaths(const std::vector<std::string>& locals);

static void setFtStatus(const std::string& s)
{
    std::lock_guard<std::mutex> lk(g_ftMutex);
    g_ftStatus = s;
}

static std::string ftStatus()
{
    std::lock_guard<std::mutex> lk(g_ftMutex);
    return g_ftStatus;
}

/* ---------------- 连接管理 ---------------- */

static bool connectDevice()
{
    clearError();

    g_link = &g_usbLink;

    if (!g_link->open()) { setError(g_link->lastError()); return false; }
    if (!g_link->handshake(true, true, 3)) {
        setError(g_link->lastError());
        g_link->close();
        return false;
    }

    if (!g_decoder.init()) { setError("解码器初始化失败"); g_link->close(); return false; }
    if (!g_audio.init())   { /* 音频失败不致命 */ }
    g_audio.setMuted(g_muted);

    g_files = new FileClient(*g_link);
    g_files->setCancelFlag(&g_ftCancel);
    g_ftCancel = false;
    g_connected = true;
    g_status = "已连接,串流中";
    g_filesLoaded = false;   /* 首次进文件页时自动列根目录 */

    g_stream = new StreamClient(*g_link);
    g_stream->start(
        [](const uint8_t* d, uint32_t l, uint8_t) { g_decoder.feed(d, l); },
        [](const uint8_t* d, uint32_t l) { g_audio.queue(d, l); },
        []() { g_connected = false; g_status = "连接已断开"; });
    return true;
}

static void disconnectDevice()
{
    if (g_stream) {
        g_stream->stop();
        delete g_stream;
        g_stream = nullptr;
    }
    g_audio.shutdown();
    g_decoder.shutdown();
    delete g_files; g_files = nullptr;
    g_link->close();
    g_connected = false;
    g_status = "未连接";
    g_entries.clear();
    g_view.clear();
    clearSelection();
    g_filesLoaded = false;
    snprintf(g_path, sizeof(g_path), "/");
}

/* ---------------- 文件页 ---------------- */

static void refreshList()
{
    if (!g_files) return;
    g_entries.clear();
    clearSelection();
    if (!g_files->list(g_path, g_entries)) {
        setError(g_files->lastError());
        return;
    }
    clearError();

    /* 目录在前,同类按名称不区分大小写排序 */
    std::stable_sort(g_entries.begin(), g_entries.end(),
        [](const FtEntry& a, const FtEntry& b) {
            if (a.type != b.type) return a.type == 1;
            const size_t n = std::min(a.name.size(), b.name.size());
            for (size_t i = 0; i < n; i++) {
                const unsigned char ca = (unsigned char)std::tolower(a.name[i]);
                const unsigned char cb = (unsigned char)std::tolower(b.name[i]);
                if (ca != cb) return ca < cb;
            }
            return a.name.size() < b.name.size();
        });

    /* 顺便刷新 SD 剩余空间(失败不影响列表) */
    uint64_t total = 0, freeSpace = 0;
    if (g_files->statFs("/", total, freeSpace)) {
        g_sdTotal = total;
        g_sdFree = freeSpace;
    } else {
        g_files->lastError();   /* 旧固件不支持 STATFS,忽略 */
    }
}

/* 过滤 + 生成当前视图 */
static void buildView()
{
    g_view.clear();
    for (int i = 0; i < (int)g_entries.size(); i++) {
        if (!g_filter[0]) { g_view.push_back(i); continue; }
        std::string name = g_entries[i].name;
        std::string needle = g_filter;
        for (char& c : name) c = (char)std::tolower((unsigned char)c);
        for (char& c : needle) c = (char)std::tolower((unsigned char)c);
        if (name.find(needle) != std::string::npos) g_view.push_back(i);
    }
}

static bool rowSelected(int idx)
{
    if (idx == g_selected) return true;
    for (int v : g_multi)
        if (v == idx) return true;
    return false;
}

static void clearSelection()
{
    g_selected = -1;
    g_multi.clear();
    g_anchor = -1;
}

static void handleRowClick(int idx)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool mod = io.KeyCtrl || io.KeySuper;

    if (io.KeyShift && g_anchor >= 0) {
        int a = -1, b = -1;
        for (int k = 0; k < (int)g_view.size(); k++) {
            if (g_view[k] == g_anchor) a = k;
            if (g_view[k] == idx) b = k;
        }
        if (a >= 0 && b >= 0) {
            if (a > b) std::swap(a, b);
            g_multi.clear();
            for (int k = a; k <= b; k++) g_multi.push_back(g_view[k]);
            g_selected = idx;
        }
        return;
    }

    if (mod) {
        if (rowSelected(idx)) {
            if (g_selected == idx) g_selected = -1;
            g_multi.erase(std::remove(g_multi.begin(), g_multi.end(), idx), g_multi.end());
        } else {
            if (g_selected >= 0) g_multi.push_back(g_selected);
            g_selected = idx;
        }
        g_anchor = idx;
        return;
    }

    g_multi.clear();
    g_selected = idx;
    g_anchor = idx;
}

/* 选中的全部条目下标(含主选中) */
static std::vector<int> selectedIndices()
{
    std::vector<int> out;
    if (g_selected >= 0 && g_selected < (int)g_entries.size()) out.push_back(g_selected);
    for (int v : g_multi)
        if (v >= 0 && v < (int)g_entries.size() &&
            std::find(out.begin(), out.end(), v) == out.end())
            out.push_back(v);
    return out;
}

static std::string joinPath(const std::string& dir, const std::string& name)
{
    std::string d = dir;
    if (d.empty()) d = "/";
    if (d.back() != '/') d += '/';
    return d + name;
}

static std::string humanSize(uint64_t bytes)
{
    char buf[64];
    if (bytes >= (uint64_t)1 << 30)
        snprintf(buf, sizeof(buf), "%.2f GB", (double)bytes / (double)((uint64_t)1 << 30));
    else if (bytes >= (uint64_t)1 << 20)
        snprintf(buf, sizeof(buf), "%.2f MB", (double)bytes / (double)((uint64_t)1 << 20));
    else if (bytes >= 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes);
    return buf;
}

/* 剩余时间:秒 -> 人类可读 */
static std::string humanTime(double sec)
{
    char buf[32];
    if (sec <= 0 || sec > 86400.0)
        return "-";
    if (sec < 60)
        snprintf(buf, sizeof(buf), "%ds", (int)sec);
    else if (sec < 3600)
        snprintf(buf, sizeof(buf), "%dm%ds", (int)(sec / 60), (int)sec % 60);
    else
        snprintf(buf, sizeof(buf), "%dh%dm", (int)(sec / 3600), ((int)sec / 60) % 60);
    return buf;
}

/* 下载目录:优先 ~/Downloads */
static std::string downloadDir()
{
    const char* home = getenv("HOME");
    if (!home) return ".";
    std::string d = std::string(home) + "/Downloads";
    struct stat st;
    if (stat(d.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return d;
    return ".";
}

/* 展开 ~ 开头路径 */
static std::string expandHome(const std::string& p)
{
    if (p.empty() || p[0] != '~') return p;
    const char* home = getenv("HOME");
    if (!home) return p;
    return std::string(home) + p.substr(1);
}

/* ---------------- 顶部栏 ---------------- */

static void drawTopBar()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 70.0f;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), COL_TOPBAR);
    dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + w, p.y + h - 1),
                IM_COL32(255, 255, 255, 14));

    /* 左侧标题 */
    dl->AddText(g_fontTitle, g_fontTitle->FontSize, ImVec2(p.x + 24, p.y + 20),
                COL_ACCENT, "SwLink");
    ImGui::PushFont(g_fontTitle);
    const float titleW = ImGui::CalcTextSize("SwLink").x;
    ImGui::PopFont();
    dl->AddText(ImVec2(p.x + 24 + titleW + 16, p.y + 29), COL_TEXT_FAINT,
                "Switch 串流 / 文件传输");

    /* 右侧:状态灯 + 状态文字 + 连接按钮 */
    const float btnW = 116.0f, btnH = 36.0f;
    const ImVec2 btnPos(p.x + w - 24 - btnW, p.y + (h - btnH) * 0.5f);

    const std::string err = errorText();
    const char* st = g_connected ? "已连接" : (err.empty() ? "未连接" : "连接失败");
    const float stW = ImGui::CalcTextSize(st).x;
    const ImVec2 stPos(btnPos.x - 30 - stW, p.y + h * 0.5f - ImGui::GetTextLineHeight() * 0.5f);

    /* 通道信息:USB */
    const char* chanInfo = g_connected ? "USB" : "";
    if (g_connected) {
        const float infoW = ImGui::CalcTextSize(chanInfo).x;
        dl->AddText(ImVec2(stPos.x - 26.0f - infoW, stPos.y), COL_TEXT_DIM, chanInfo);
    }

    dl->AddCircleFilled(ImVec2(stPos.x - 14, p.y + h * 0.5f), 5.0f,
                        g_connected ? COL_OK : COL_ERR);
    dl->AddText(stPos, g_connected ? COL_TEXT_DIM
                                   : (err.empty() ? COL_TEXT_FAINT : COL_ERR), st);

    ImGui::BeginDisabled(g_ftBusy || g_nxBusy.load());
    ImGui::SetCursorScreenPos(btnPos);
    if (g_connected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.24f, 0.28f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.25f, 0.27f, 1.0f));
        if (ImGui::Button("断开", ImVec2(btnW, btnH))) disconnectDevice();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.45f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.32f, 0.52f, 0.98f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.22f, 0.40f, 0.80f, 1.0f));
        if (ImGui::Button("连接 Switch", ImVec2(btnW, btnH)))
            connectDevice();
        ImGui::PopStyleColor(3);
    }
    ImGui::EndDisabled();

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

/* ---------------- 侧边栏 ---------------- */

static void navItem(const char* label, int page)
{
    const bool selected = (g_page == page);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();

    ImGui::PushStyleColor(ImGuiCol_Header, selected ? COL_ACCENT_SOFT : IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(255, 255, 255, 18));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(255, 255, 255, 26));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));

    if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_None, ImVec2(0, 44)))
        g_page = page;

    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    if (selected) {
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImVec2(p0.x - 16, p0.y + 12), ImVec2(p0.x - 13, p0.y + 32),
            COL_ACCENT, 2.0f);
    }
}

static void drawSidebar()
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y;

    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), COL_SIDEBAR);

    ImGui::SetCursorScreenPos(ImVec2(p.x + 34, p.y + 22));
    dl->AddText(ImVec2(p.x + 34, p.y + 22), COL_TEXT_FAINT, "功能");

    ImGui::SetCursorScreenPos(ImVec2(p.x + 18, p.y + 46));
    ImGui::BeginGroup();
    navItem("串流画面", 0);
    navItem("SD 卡文件", 1);
    navItem("NRO 推送", 2);
    navItem("使用帮助", 3);
    ImGui::EndGroup();

    /* 底部:连接状态 + 版本 */
    dl->AddCircleFilled(ImVec2(p.x + 28, p.y + h - 60), 4.0f,
                        g_connected ? COL_OK : COL_ERR);
    dl->AddText(ImVec2(p.x + 40, p.y + h - 67),
                g_connected ? COL_TEXT_DIM : COL_TEXT_FAINT,
                g_connected ? "已连接" : "未连接");

    dl->AddText(ImVec2(p.x + 24, p.y + h - 40), COL_TEXT_FAINT,
                "v0.2  ·  USB " "16C0:27DA");
    dl->AddText(ImVec2(p.x + 24, p.y + h - 24), COL_TEXT_FAINT,
                "H.264 720p30");
}

/* ---------------- 串流页 ---------------- */

/* ---------------- 独立串流窗口 ---------------- */

static void closeVideoWindow()
{
    if (g_vtex)  { SDL_DestroyTexture(g_vtex);  g_vtex = nullptr; }
    if (g_vrend) { SDL_DestroyRenderer(g_vrend); g_vrend = nullptr; }
    if (g_vwin)  { SDL_DestroyWindow(g_vwin);   g_vwin = nullptr; }
    g_vtexW = g_vtexH = 0;
    g_vframe = 0;
}

static bool openVideoWindow()
{
    if (g_vwin) return true;

    const int w = g_decoder.width()  > 0 ? g_decoder.width()  : 1280;
    const int h = g_decoder.height() > 0 ? g_decoder.height() : 720;

    g_vwin = SDL_CreateWindow("SwLink 串流画面",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              w, h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_vwin) return false;

    g_vrend = SDL_CreateRenderer(g_vwin, -1,
                                 SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_vrend) {
        closeVideoWindow();
        return false;
    }
    return true;
}

/* 每帧调用:纹理跟随分辨率变化,有新帧才上传,按比例居中显示 */
static void drawVideoWindow()
{
    if (!g_vwin || !g_vrend) return;

    const int dw = g_decoder.width();
    const int dh = g_decoder.height();
    if (dw > 0 && dh > 0 && (!g_vtex || dw != g_vtexW || dh != g_vtexH)) {
        if (g_vtex) SDL_DestroyTexture(g_vtex);
        g_vtex = SDL_CreateTexture(g_vrend, SDL_PIXELFORMAT_IYUV,
                                   SDL_TEXTUREACCESS_STREAMING, dw, dh);
        if (g_vtex) SDL_SetTextureScaleMode(g_vtex, SDL_ScaleModeLinear);
        g_vtexW = dw;
        g_vtexH = dh;
    }

    const uint64_t frames = g_decoder.framesDecoded();
    if (g_vtex && frames != g_vframe) {
        g_vframe = frames;
        g_decoder.uploadInto(g_vtex);
    }

    SDL_SetRenderDrawColor(g_vrend, 9, 10, 13, 255);
    SDL_RenderClear(g_vrend);

    if (g_vtex && g_vtexW > 0 && g_vtexH > 0) {
        /* 用渲染器输出尺寸而不是窗口尺寸:Retina 下二者差一个缩放倍数 */
        int ww = 0, wh = 0;
        SDL_GetRendererOutputSize(g_vrend, &ww, &wh);
        const float aspect = (float)g_vtexW / (float)g_vtexH;
        float dwp = (float)ww, dhp = (float)wh;
        if (dwp / dhp > aspect) dwp = dhp * aspect;
        else                    dhp = dwp / aspect;

        SDL_Rect dst = { (int)((ww - dwp) * 0.5f), (int)((wh - dhp) * 0.5f),
                         (int)dwp, (int)dhp };
        SDL_RenderCopy(g_vrend, g_vtex, nullptr, &dst);
    }

    SDL_RenderPresent(g_vrend);
}

static void drawStreamPage(SDL_Renderer* renderer)
{
    /* 标题行 */
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("串流画面");
    ImGui::PopFont();

    ImGui::SameLine(0, 18);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
    if (g_connected) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM));
        ImGui::Text("%dx%d  ·  %.1f fps  ·  %llu 帧",
                    g_decoder.width(), g_decoder.height(), g_fps,
                    (unsigned long long)g_decoder.framesDecoded());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT_FAINT));
        ImGui::TextUnformatted("未连接");
        ImGui::PopStyleColor();
    }

    /* 右侧操作:独立窗口 / 全屏 / 静音 */
    {
        const float wV = 100.0f, wF = 78.0f, wM = 78.0f;
        const float total = wV + wF + wM + 16.0f;
        const float rightX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total;
        ImGui::SetCursorPosX(rightX);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6);

        if (ImGui::Button(g_vwin ? "关闭窗口" : "独立窗口", ImVec2(wV, 30))) {
            if (g_vwin) closeVideoWindow();
            else        openVideoWindow();
        }
        ImGui::SameLine(0, 8);
        if (ImGui::Button("全屏", ImVec2(wF, 30))) {
            if (g_window) {
                const Uint32 flags = SDL_GetWindowFlags(g_window);
                SDL_SetWindowFullscreen(g_window,
                    (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }
        ImGui::SameLine(0, 8);
        if (ImGui::Button(g_muted ? "取消静音" : "静音", ImVec2(wM, 30))) {
            g_muted = !g_muted;
            g_audio.setMuted(g_muted);
        }
    }

    ImGui::Spacing();

    /* 画面面板:有错误时在底部预留一行给错误提示 */
    const std::string err = errorText();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float errH = err.empty() ? 0.0f : 34.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##video", ImVec2(0, avail.y - errH), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ip = ImGui::GetCursorScreenPos();
    const ImVec2 inner = ImGui::GetContentRegionAvail();

    dl->AddRectFilled(ip, ImVec2(ip.x + inner.x, ip.y + inner.y), COL_VIDEO_BG, 10.0f);

    /* 独立窗口开着时主窗口不再上传/绘制视频,省一路重复拷贝 */
    const bool ownWindow = (g_vwin != nullptr);
    SDL_Texture* tex = (g_connected && !ownWindow) ? g_decoder.uploadIfDirty(renderer) : nullptr;

    if (ownWindow) {
        const char* msg  = "画面已在独立窗口显示";
        const char* hint = "点「关闭窗口」可回到这里显示";
        const float hintW = ImGui::CalcTextSize(hint).x;
        const float cy = ip.y + inner.y * 0.42f;
        ImGui::PushFont(g_fontTitle);
        const float msgW = ImGui::CalcTextSize(msg).x;
        dl->AddText(g_fontTitle, g_fontTitle->FontSize,
                    ImVec2(ip.x + (inner.x - msgW) * 0.5f, cy), COL_TEXT_FAINT, msg);
        ImGui::PopFont();
        dl->AddText(ImVec2(ip.x + (inner.x - hintW) * 0.5f, cy + 36), COL_TEXT_FAINT, hint);
    } else if (tex && g_decoder.width() > 0 && g_decoder.height() > 0) {
        const float ar = (float)g_decoder.width() / (float)g_decoder.height();
        float w = inner.x, h = w / ar;
        if (h > inner.y) { h = inner.y; w = h * ar; }
        const ImVec2 o(ip.x + (inner.x - w) * 0.5f, ip.y + (inner.y - h) * 0.5f);
        dl->AddImage((ImTextureID)tex, o, ImVec2(o.x + w, o.y + h));
    } else {
        const char* msg  = g_connected ? "等待画面..." : "尚未连接";
        const char* hint = g_connected
            ? "请进入游戏并停留在游戏画面(主菜单不产生录制输出)"
            : "点击右上角「连接 Switch」开始串流";
        const float hw = ImGui::CalcTextSize(hint).x;
        const float cy = ip.y + inner.y * 0.42f;
        ImGui::PushFont(g_fontTitle);
        const float tw = ImGui::CalcTextSize(msg).x;
        dl->AddText(g_fontTitle, g_fontTitle->FontSize,
                    ImVec2(ip.x + (inner.x - tw) * 0.5f, cy), COL_TEXT_FAINT, msg);
        ImGui::PopFont();
        dl->AddText(ImVec2(ip.x + (inner.x - hw) * 0.5f, cy + 36), COL_TEXT_FAINT, hint);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (!err.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_ERR));
        ImGui::TextWrapped("%s", err.c_str());
        ImGui::PopStyleColor();
    }
}

/* ---------------- 文件页 ---------------- */

static void drawBreadcrumb()
{
    if (ImGui::Button("SD 根目录")) {
        snprintf(g_path, sizeof(g_path), "/");
        refreshList();
    }

    std::string rest = g_path;
    if (!rest.empty() && rest[0] == '/') rest.erase(0, 1);

    std::string acc = "/";
    while (!rest.empty()) {
        const size_t pos = rest.find('/');
        const std::string seg = (pos == std::string::npos) ? rest : rest.substr(0, pos);
        rest = (pos == std::string::npos) ? std::string() : rest.substr(pos + 1);
        if (seg.empty()) continue;

        ImGui::SameLine(0, 6);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT_FAINT), "/");
        ImGui::SameLine(0, 6);

        const std::string target = acc + seg;
        if (ImGui::SmallButton(seg.c_str())) {
            snprintf(g_path, sizeof(g_path), "%s", target.c_str());
            refreshList();
        }
        acc = target + "/";
    }
}

/* 新建目录 / 重命名 / 上传 / 删除确认 */
static int g_modal = 0;                 /* 1=新建目录 2=重命名 3=上传 4=删除 */
static char g_modalInput1[512] = {0};
static char g_modalInput2[256] = {0};
static std::string g_modalTarget;
static std::vector<std::pair<std::string, bool>> g_modalTargets;   /* (路径, 是否目录) */
static bool g_modalIsDir = false;

/* 列表里的条目图标(目录=文件夹,文件=带折角的纸) */
static void drawEntryIcon(ImDrawList* dl, ImVec2 p, bool isDir, ImU32 col)
{
    if (isDir) {
        dl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + 16, p.y + 15), col, 2.0f);
        dl->AddRectFilled(ImVec2(p.x, p.y + 2), ImVec2(p.x + 7, p.y + 6), col, 1.5f);
    } else {
        dl->AddRect(ImVec2(p.x + 2, p.y + 2), ImVec2(p.x + 14, p.y + 16), col, 2.0f, 0, 1.2f);
        dl->AddLine(ImVec2(p.x + 9, p.y + 2), ImVec2(p.x + 14, p.y + 7), col, 1.2f);
        dl->AddLine(ImVec2(p.x + 9, p.y + 2), ImVec2(p.x + 9, p.y + 7), col, 1.2f);
        dl->AddLine(ImVec2(p.x + 9, p.y + 7), ImVec2(p.x + 14, p.y + 7), col, 1.2f);
    }
}

/* ---------------- 递归传输 ---------------- */

struct TransferJob {
    std::string remote;
    std::string local;
    uint64_t size = 0;
    bool isDir = false;
};

static std::string baseName(const std::string& p)
{
    const size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? p : p.substr(slash + 1);
}

/* 递归展开远端目录(先压入目录任务,再压入文件任务) */
static bool collectRemote(const std::string& remote, const std::string& local,
                          std::vector<TransferJob>& jobs)
{
    std::vector<FtEntry> entries;
    if (!g_files->list(remote, entries)) return false;

    for (const FtEntry& e : entries) {
        if (g_ftCancel.load()) return true;

        TransferJob job;
        job.remote = joinPath(remote, e.name);
        job.local = local + "/" + e.name;
        if (e.type == 1) {
            job.isDir = true;
            jobs.push_back(job);
            if (!collectRemote(job.remote, job.local, jobs)) return false;
        } else {
            job.size = e.size;
            jobs.push_back(job);
        }
    }
    return true;
}

/* 递归展开本地目录 */
static bool collectLocal(const std::string& local, const std::string& remote,
                         std::vector<TransferJob>& jobs)
{
    struct stat st;
    if (stat(local.c_str(), &st) != 0) return false;

    if (S_ISDIR(st.st_mode)) {
        TransferJob job;
        job.remote = remote;
        job.local = local;
        job.isDir = true;
        jobs.push_back(job);

        DIR* d = opendir(local.c_str());
        if (!d) return false;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            if (g_ftCancel.load()) { closedir(d); return true; }
            if (!collectLocal(local + "/" + ent->d_name,
                              joinPath(remote, ent->d_name), jobs)) {
                closedir(d);
                return false;
            }
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        TransferJob job;
        job.remote = remote;
        job.local = local;
        job.size = (uint64_t)st.st_size;
        jobs.push_back(job);
    }
    return true;
}

/* 在工作线程执行任务列表(上传或下载),结束时复位状态 */
static void runTransfer(std::vector<TransferJob> jobs, bool upload)
{
    uint64_t total = 0;
    int fileCount = 0;
    for (const auto& j : jobs)
        if (!j.isDir) { total += j.size; fileCount++; }

    g_ftTotal = total;
    g_ftDone = 0;

    uint64_t done = 0;
    int okCount = 0, failCount = 0;
    std::string firstErr;

    for (const TransferJob& j : jobs) {
        if (g_ftCancel.load()) break;

        if (j.isDir) {
            if (upload) g_files->mkdir(j.remote);
            else mkdir(j.local.c_str(), 0755);
            continue;
        }

        setFtStatus(std::string(upload ? "正在上传 " : "正在下载 ") + baseName(j.remote) +
                    "  (" + std::to_string(okCount + failCount + 1) + "/" +
                    std::to_string(fileCount) + ")");

        const bool ok = upload
            ? g_files->writeFile(j.local, j.remote,
                [&done](uint64_t d, uint64_t) { g_ftDone = done + d; })
            : g_files->readFile(j.remote, j.local,
                [&done](uint64_t d, uint64_t) { g_ftDone = done + d; });

        done += j.size;
        g_ftDone = done;

        if (ok) {
            okCount++;
        } else {
            failCount++;
            if (firstErr.empty()) firstErr = g_files->lastError();
        }
    }

    if (g_ftCancel.load())
        setFtStatus(upload ? "上传已取消" : "下载已取消");
    else if (failCount == 0)
        setFtStatus((upload ? "已上传 " : "已下载 ") + std::to_string(okCount) + " 个文件" +
                    (upload ? "" : " 到 " + downloadDir()));
    else {
        setFtStatus(std::to_string(failCount) + " 项失败: " + firstErr);
        setError(firstErr);
    }

    g_ftDone = 0;
    g_ftTotal = 0;
    g_ftCancel = false;
    g_ftBusy = false;
    g_refreshRequest = true;
}

/* 下载选中项(目录递归打包下载到 ~/Downloads) */
static void beginDownloadSelected()
{
    if (!g_files || g_ftBusy) return;

    struct Root { std::string remote; bool isDir; };
    std::vector<Root> roots;
    for (int idx : selectedIndices())
        roots.push_back({joinPath(g_path, g_entries[idx].name),
                         g_entries[idx].type == 1});
    if (roots.empty()) return;

    clearError();
    g_ftBusy = true;
    g_ftCancel = false;
    std::thread([roots]() {
        std::vector<TransferJob> jobs;
        for (const Root& r : roots) {
            TransferJob job;
            job.remote = r.remote;
            job.local = downloadDir() + "/" + baseName(r.remote);

            if (r.isDir) {
                job.isDir = true;
                jobs.push_back(job);
                if (!collectRemote(job.remote, job.local, jobs)) {
                    setFtStatus("展开目录失败: " + g_files->lastError());
                    g_ftBusy = false;
                    return;
                }
            } else {
                uint8_t type = 0;
                uint64_t size = 0;
                if (!g_files->stat(r.remote, type, size)) {
                    setFtStatus("读取文件信息失败: " + g_files->lastError());
                    g_ftBusy = false;
                    return;
                }
                job.size = size;
                jobs.push_back(job);
            }
        }
        runTransfer(std::move(jobs), false);
    }).detach();
}

/* 上传本地文件/目录(目录会在远端同名创建) */
static void beginUploadPaths(const std::vector<std::string>& locals)
{
    if (!g_files || g_ftBusy || locals.empty()) return;
    const std::string dir = g_path;

    clearError();
    g_ftBusy = true;
    g_ftCancel = false;
    std::thread([locals, dir]() {
        std::vector<TransferJob> jobs;
        for (const auto& local : locals) {
            if (!collectLocal(local, joinPath(dir, baseName(local)), jobs)) {
                setFtStatus("无法读取本地路径: " + local);
                setError("无法读取本地路径: " + local);
                g_ftBusy = false;
                return;
            }
        }
        runTransfer(std::move(jobs), true);
    }).detach();
}

static void startUpload()
{
    if (!g_files) return;
    const std::string local = expandHome(g_modalInput1);
    if (local.empty()) return;

    /* 指定了目标文件名:单文件上传并改名 */
    const std::string name = g_modalInput2;
    if (name.empty()) {
        ImGui::CloseCurrentPopup();
        beginUploadPaths({ local });
        return;
    }

    struct stat st;
    if (stat(local.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        setError("本地文件不存在: " + local);
        return;
    }

    TransferJob job;
    job.remote = joinPath(g_path, name);
    job.local = local;
    job.size = (uint64_t)st.st_size;

    clearError();
    g_ftBusy = true;
    g_ftCancel = false;
    ImGui::CloseCurrentPopup();
    std::thread([job]() { runTransfer({ job }, true); }).detach();
}

static void drawModals()
{
    /* 新建目录 */
    if (ImGui::BeginPopupModal("新建目录", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM), "位置: %s", g_path);
        ImGui::SetNextItemWidth(360);
        ImGui::InputTextWithHint("##dirname", "目录名", g_modalInput1, sizeof(g_modalInput1));
        ImGui::Spacing();
        if (ImGui::Button("创建", ImVec2(110, 32)) && g_modalInput1[0]) {
            if (g_files && g_files->mkdir(joinPath(g_path, g_modalInput1))) {
                setFtStatus(std::string("已创建目录: ") + g_modalInput1);
                g_modalInput1[0] = 0;
                refreshList();
                ImGui::CloseCurrentPopup();
            } else {
                setError(g_files ? g_files->lastError() : "未连接");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(110, 32))) {
            g_modalInput1[0] = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* 重命名 */
    if (ImGui::BeginPopupModal("重命名", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM), "当前: %s", g_modalTarget.c_str());
        ImGui::SetNextItemWidth(360);
        ImGui::InputTextWithHint("##newname", "新名称", g_modalInput1, sizeof(g_modalInput1));
        ImGui::Spacing();
        if (ImGui::Button("确定", ImVec2(110, 32)) && g_modalInput1[0]) {
            if (g_files && g_files->rename(g_modalTarget, joinPath(g_path, g_modalInput1))) {
                setFtStatus("已重命名");
                g_modalInput1[0] = 0;
                refreshList();
                ImGui::CloseCurrentPopup();
            } else {
                setError(g_files ? g_files->lastError() : "未连接");
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(110, 32))) {
            g_modalInput1[0] = 0;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* 上传 */
    if (ImGui::BeginPopupModal("上传文件", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM), "目标目录: %s", g_path);
        ImGui::SetNextItemWidth(460);
        ImGui::InputTextWithHint("##localpath", "本地文件路径(可拖入或填 ~/Downloads/xxx)",
                                g_modalInput1, sizeof(g_modalInput1));
        ImGui::SetNextItemWidth(460);
        ImGui::InputTextWithHint("##remoteName", "上传后的文件名(留空=用原文件名)",
                                g_modalInput2, sizeof(g_modalInput2));
        ImGui::Spacing();
        ImGui::BeginDisabled(g_modalInput1[0] == 0);
        if (ImGui::Button("上传", ImVec2(110, 32))) startUpload();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(110, 32))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    /* 删除确认 */
    if (ImGui::BeginPopupModal("删除确认", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int count = (int)g_modalTargets.size();
        if (count > 1) {
            ImGui::Text("确定删除以下 %d 项?", count);
            for (int i = 0; i < count && i < 6; i++)
                ImGui::BulletText("%s", g_modalTargets[i].first.c_str());
            if (count > 6)
                ImGui::BulletText("…共 %d 项", count);
        } else {
            ImGui::Text("确定删除%s?", g_modalIsDir ? "目录(含全部内容)" : "文件");
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_ERR), "%s",
                               g_modalTargets.empty() ? "" : g_modalTargets[0].first.c_str());
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.22f, 0.22f, 1.0f));
        if (ImGui::Button("删除", ImVec2(110, 32))) {
            int okCount = 0;
            std::string firstErr;
            for (const auto& target : g_modalTargets) {
                if (g_files && g_files->remove(target.first, target.second))
                    okCount++;
                else if (firstErr.empty())
                    firstErr = g_files ? g_files->lastError() : "未连接";
            }
            setFtStatus(firstErr.empty()
                ? ("已删除 " + std::to_string(okCount) + " 项")
                : ("部分删除失败: " + firstErr));
            g_modalTargets.clear();
            refreshList();
            ImGui::CloseCurrentPopup();
        }
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(110, 32))) {
            g_modalTargets.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void drawFilePage()
{
    if (!g_connected || !g_files) {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* msg = "尚未连接,请先连接 Switch";
        const float w = ImGui::CalcTextSize(msg).x;
        ImGui::SetCursorPos(ImVec2((avail.x - w) * 0.5f, avail.y * 0.45f));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT_FAINT), "%s", msg);
        const std::string err = errorText();
        if (!err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_ERR));
            ImGui::TextWrapped("%s", err.c_str());
            ImGui::PopStyleColor();
        }
        return;
    }

    /* 首次进入自动列根目录;上传完成后自动刷新 */
    if (!g_filesLoaded) {
        g_filesLoaded = true;
        refreshList();
    }
    if (g_refreshRequest.exchange(false))
        refreshList();

    buildView();

    /* 面包屑 + 刷新 + 搜索 */
    drawBreadcrumb();
    ImGui::SameLine(0, 10);
    if (ImGui::Button("刷新"))
        refreshList();

    {
        const float filterW = 210.0f;
        const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - filterW;
        ImGui::SameLine();
        if (x > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(x);
        ImGui::SetNextItemWidth(filterW);
        if (ImGui::InputTextWithHint("##filter", "搜索名称", g_filter, sizeof(g_filter)))
            clearSelection();
    }

    ImGui::Spacing();

    const std::vector<int> sel = selectedIndices();
    const bool hasSel = !sel.empty();
    bool selHasDir = false;
    for (int idx : sel) {
        if (g_entries[idx].type == 1) selHasDir = true;
    }

    /* 操作按钮行 */
    ImGui::BeginDisabled(g_ftBusy || !hasSel);
    if (ImGui::Button(sel.size() > 1 ? "下载选中" : "下载", ImVec2(0, 30)))
        beginDownloadSelected();
    ImGui::EndDisabled();
    ImGui::SameLine();

    {
        const std::string up = g_path;
        ImGui::BeginDisabled(g_ftBusy || up == "/");
        if (ImGui::Button("上一级", ImVec2(0, 30))) {
            std::string p = up;
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            const size_t pos = p.find_last_of('/');
            p = (pos == 0 || pos == std::string::npos) ? "/" : p.substr(0, pos);
            snprintf(g_path, sizeof(g_path), "%s", p.c_str());
            refreshList();
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();

    ImGui::BeginDisabled(g_ftBusy);
    if (ImGui::Button("上传", ImVec2(0, 30))) {
        g_modalInput1[0] = 0;
        g_modalInput2[0] = 0;
        ImGui::OpenPopup("上传文件");
    }
    ImGui::SameLine();
    if (ImGui::Button("新建目录", ImVec2(0, 30))) {
        g_modalInput1[0] = 0;
        ImGui::OpenPopup("新建目录");
    }
    ImGui::EndDisabled();
    ImGui::SameLine();

    ImGui::BeginDisabled(g_ftBusy || sel.size() != 1);
    if (ImGui::Button("重命名", ImVec2(0, 30)) && sel.size() == 1) {
        snprintf(g_modalInput1, sizeof(g_modalInput1), "%s", g_entries[sel[0]].name.c_str());
        g_modalTarget = joinPath(g_path, g_entries[sel[0]].name);
        ImGui::OpenPopup("重命名");
    }
    ImGui::SameLine();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.30f, 0.16f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.20f, 0.22f, 1.0f));
    if (ImGui::Button("删除", ImVec2(0, 30)) && hasSel) {
        g_modalTargets.clear();
        for (int idx : sel)
            g_modalTargets.emplace_back(joinPath(g_path, g_entries[idx].name),
                                        g_entries[idx].type == 1);
        g_modalIsDir = selHasDir;
        ImGui::OpenPopup("删除确认");
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();

    ImGui::Spacing();

    /* 文件列表 */
    const std::string err = errorText();
    const float bottomH = (g_ftBusy.load() ? 36.0f : 70.0f) + (err.empty() ? 0.0f : 24.0f);
    const float listH = ImGui::GetContentRegionAvail().y - bottomH;
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;

    int enterIdx = -1;

    if (ImGui::BeginTable("##files", 3, tf, ImVec2(0, listH > 60 ? listH : 60))) {
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch |
                                      ImGuiTableColumnFlags_DefaultSort, 0.0f, 1);
        ImGui::TableSetupColumn("大小", ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_PreferSortDescending, 110.0f, 2);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        /* 点击表头排序(目录始终排在前面) */
        if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
            if (specs->SpecsCount > 0 && specs->SpecsDirty) {
                const int col = specs->Specs[0].ColumnIndex;
                const bool asc =
                    specs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                std::stable_sort(g_entries.begin(), g_entries.end(),
                    [col, asc](const FtEntry& a, const FtEntry& b) {
                        if (a.type != b.type) return a.type == 1;
                        if (col == 2) return asc ? a.size < b.size : a.size > b.size;
                        std::string la = a.name, lb = b.name;
                        for (char& c : la) c = (char)std::tolower((unsigned char)c);
                        for (char& c : lb) c = (char)std::tolower((unsigned char)c);
                        return asc ? la < lb : la > lb;
                    });
                specs->SpecsDirty = false;
                clearSelection();
                buildView();
            }
        }

        if (g_view.empty()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Dummy(ImVec2(0, 2));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", g_entries.empty() ? "此目录为空" : "没有匹配的条目");
            ImGui::TableNextColumn();
        }

        for (int r = 0; r < (int)g_view.size(); r++) {
            const int i = g_view[r];
            const FtEntry& e = g_entries[i];

            ImGui::PushID(i);
            ImGui::TableNextRow();

            if (rowSelected(i))
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, COL_ROW_SEL);

            /* 类型列:手绘图标 */
            ImGui::TableNextColumn();
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                drawEntryIcon(ImGui::GetWindowDrawList(),
                              ImVec2(p.x + 4, p.y + ImGui::GetTextLineHeight() * 0.5f - 9),
                              e.type == 1,
                              e.type == 1 ? COL_ACCENT : COL_TEXT_FAINT);
                ImGui::Dummy(ImVec2(20, ImGui::GetTextLineHeight()));
            }

            /* 名称列 */
            ImGui::TableNextColumn();
            if (ImGui::Selectable(e.name.c_str(), rowSelected(i),
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0, 26))) {
                handleRowClick(i);
                if (ImGui::IsMouseDoubleClicked(0)) {
                    if (e.type == 1) {
                        enterIdx = i;
                    } else {
                        clearSelection();
                        g_selected = i;
                        beginDownloadSelected();
                    }
                }
            }

            /* 大小列 */
            ImGui::TableNextColumn();
            if (e.type == 2) {
                const std::string sz = humanSize(e.size);
                const float tw = ImGui::CalcTextSize(sz.c_str()).x;
                const float availW = ImGui::GetContentRegionAvail().x;
                const float dx = (availW - tw - 8.0f) > 0.f ? (availW - tw - 8.0f) : 0.f;
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + dx);
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM), "%s", sz.c_str());
            } else {
                ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT_FAINT), "—");
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (enterIdx >= 0) {
        const std::string target = joinPath(g_path, g_entries[enterIdx].name);
        snprintf(g_path, sizeof(g_path), "%s", target.c_str());
        refreshList();
    }

    /* 底部:进度 / 条目数 / 状态 / SD 空间 / 错误 */
    ImGui::Spacing();
    const bool busy = g_ftBusy.load();

    /* 速度/剩余时间的采样状态(跨帧保留) */
    static Uint64 s_lastTick = 0;
    static uint64_t s_lastDone = 0;
    static double s_speed = 0.0;

    if (busy) {
        const Uint64 now = SDL_GetTicks();
        const uint64_t done = g_ftDone.load();
        const uint64_t total = g_ftTotal.load();

        if (s_lastTick == 0) { s_lastTick = now; s_lastDone = done; }
        if (now - s_lastTick >= 300) {
            s_speed = (double)(done - s_lastDone) * 1000.0 / (double)(now - s_lastTick);
            s_lastTick = now;
            s_lastDone = done;
        }

        char label[160];
        float frac = -1.0f;
        if (total > 0) {
            frac = (float)((double)done / (double)total);
            const double remain = (double)(total - done);
            const double eta = s_speed > 0 ? remain / s_speed : 0.0;
            snprintf(label, sizeof(label), "%s / %s   ·   %.1f MB/s   ·   剩余 %s",
                     humanSize(done).c_str(), humanSize(total).c_str(),
                     s_speed / (1024.0 * 1024.0), humanTime(eta).c_str());
        } else {
            snprintf(label, sizeof(label), "正在统计文件…");
        }

        ImGui::ProgressBar(frac, ImVec2(ImGui::GetContentRegionAvail().x - 92, 18), label);
        ImGui::SameLine(0, 10);
        if (ImGui::Button("取消", ImVec2(82, 22)))
            g_ftCancel = true;
    } else {
        s_lastTick = 0;
        s_lastDone = 0;
        s_speed = 0.0;

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM));
        ImGui::Text("%d 个条目", (int)g_entries.size());
        const std::string status = ftStatus();
        if (!status.empty()) {
            ImGui::SameLine(0, 12);
            ImGui::TextDisabled("%s", status.c_str());
        }
        if (g_sdTotal.load() > 0) {
            ImGui::SameLine(0, 12);
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT_FAINT),
                               "SD 剩余 %s / %s",
                               humanSize(g_sdFree.load()).c_str(),
                               humanSize(g_sdTotal.load()).c_str());
        }
        ImGui::PopStyleColor();

        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT_FAINT));
        ImGui::Text("可多选(Cmd/Shift) · 双击目录进入 · 双击文件下载 · 拖入文件或文件夹即上传");
        ImGui::PopStyleColor();
    }

    if (!err.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_ERR));
        ImGui::TextWrapped("%s", err.c_str());
        ImGui::PopStyleColor();
    }

    drawModals();
}

/* ---------------- 帮助页 ---------------- */

static void helpRow(const char* title, const char* body)
{
    ImGui::Spacing();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_TEXT), "%s", title);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM));
    ImGui::TextWrapped("%s", body);
    ImGui::PopStyleColor();
}

/* ---------------- NRO 推送页 ---------------- */

static bool endsWithNro(const std::string& s)
{
    return s.size() > 4 && s.compare(s.size() - 4, 4, ".nro") == 0;
}

static void startNxlinkPush()
{
    if (g_nxBusy.load())
        return;

    if (!g_connected || !g_files) {
        setNxStatus("请先连接 Switch(USB)");
        return;
    }
    if (g_nxNroPath[0] == '\0') {
        setNxStatus("请先选择本地的 .nro 文件(也可直接拖进窗口)");
        return;
    }
    if (!endsWithNro(g_nxSdPath)) {
        setNxStatus("Switch 上的保存路径必须以 .nro 结尾");
        return;
    }

    const std::string nro = g_nxNroPath;
    const std::string sd = g_nxSdPath;

    g_nxBusy = true;
    g_nxPercent = 0;
    setNxStatus("正在上传...");
    appendNxLog("推送 " + nro + " -> SD:" + sd);

    std::thread([nro, sd]() {
        FileClient* files = g_files;
        if (!files) {
            g_nxBusy = false;
            return;
        }

        /* hbmenu 只列 switch 目录,目录不存在就先建一个 */
        if (sd.rfind("switch/", 0) == 0)
            files->mkdir("switch");

        const bool ok = files->writeFile(nro, sd, [](uint64_t done, uint64_t total) {
            if (total > 0)
                g_nxPercent = (int)(done * 100 / total);
        });

        if (ok) {
            setNxStatus("已写入 SD 卡");
            appendNxLog("完成:SD:" + sd);
            appendNxLog("在 Switch 上按住 R 启动 hbmenu,进入 switch 目录点选该文件即可运行");
        } else {
            const std::string err = files->lastError();
            setNxStatus("推送失败: " + err);
            appendNxLog("失败: " + err);
        }
        g_nxBusy = false;
    }).detach();
}

static void drawNxlinkPage()
{
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("NRO 推送(USB)");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM));
    ImGui::TextWrapped(
        "通过 USB 把本地 .nro 直接写入 Switch 的 SD 卡,不需要网络。"
        "写入后到 Switch 上启动 hbmenu(按住 R 键进入全内存模式),在 switch 目录里点选它即可运行。");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::BeginDisabled(g_nxBusy.load());

    ImGui::SetNextItemWidth(520);
    ImGui::InputTextWithHint("##nxnro", "本地 .nro 文件路径(可拖拽文件到窗口)",
                             g_nxNroPath, sizeof(g_nxNroPath));

    ImGui::SetNextItemWidth(520);
    ImGui::InputTextWithHint("##nxdest", "Switch SD 卡上的保存路径,如 switch/xxx.nro",
                             g_nxSdPath, sizeof(g_nxSdPath));

    ImGui::Spacing();
    if (ImGui::Button(g_nxBusy.load() ? "上传中..." : "推送到 SD 卡", ImVec2(140, 34)))
        startNxlinkPush();
    ImGui::SameLine();
    if (ImGui::Button("清空记录", ImVec2(96, 34))) {
        std::lock_guard<std::mutex> lk(g_nxStatusMutex);
        g_nxLog.clear();
    }

    ImGui::EndDisabled();

    ImGui::SameLine();
    const std::string status = nxStatus();
    if (g_nxBusy.load())
        ImGui::TextDisabled("%s %d%%", status.c_str(), g_nxPercent.load());
    else if (!status.empty())
        ImGui::TextDisabled("%s", status.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild("##nxlog", ImVec2(0, avail.y > 40 ? avail.y : 40), true,
                      ImGuiWindowFlags_HorizontalScrollbar);

    const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    const std::string log = nxLog();
    if (log.empty())
        ImGui::TextDisabled("推送记录会显示在这里");
    else
        ImGui::TextUnformatted(log.c_str());
    if (atBottom)
        ImGui::SetScrollHereY(1.0f);

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

static void drawHelpPage()
{
    ImGui::PushFont(g_fontTitle);
    ImGui::TextUnformatted("使用帮助");
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT_DIM));
    ImGui::TextWrapped(
        "SwLink 通过 USB 连接 Switch(大气层)与 macOS:串流游戏画面+声音,"
        "并可浏览/上传/下载 SD 卡文件。Switch 端需要安装 SwLink sysmodule,"
        "USB 设备 ID 为 16C0:27DA。");
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    helpRow("没有画面?",
            "只有支持系统录屏(长按截图键)的游戏才能采集。主菜单和不支持的游戏会让 grc:d 一直"
            "阻塞,属正常现象;可安装 dvr-patches 解锁全部游戏。连接后请进入游戏并停留在游戏画面。");

    helpRow("连接失败/找不到设备?",
            "先在 Switch 上确认 sysmodule 已启动(关机重开 或 Tesla 菜单里启用),"
            "再确认 USB 线是数据线。串流与文件传输共用同一 USB 连接,不要与 SysDVR 同时运行。");

    helpRow("文件页操作",
            "单击选中,按住 Cmd 或 Shift 可多选;双击目录进入,双击文件直接下载到 ~/Downloads。"
            "把本地文件拖进窗口即可上传到当前目录。删除目录会连同内容一并删除,请谨慎操作。");

    helpRow("画质与延迟",
            "视频由 Switch 系统硬件编码,固定 720p30、码率不可调;若出现卡顿,优先检查 USB 线材与接口。"
            "音频为 PCM 48kHz 立体声,可在串流页切换静音。");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(COL_TEXT_FAINT));
    ImGui::Text("版本 v0.2  ·  协议版本 %s  ·  项目地址 github.com/xbw12138/SwitchTools",
                SWLINK_PROTO_VERSION);
    ImGui::PopStyleColor();
}

/* ---------------- main ---------------- */

int main(int, char**)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("SwLink",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1320, 860,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window || !renderer) {
        fprintf(stderr, "SDL 窗口/渲染器创建失败\n");
        return 1;
    }
    g_window = window;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyTheme();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    loadFonts();

    bool running = true;
    std::vector<std::string> dropped;          /* 拖入的文件,下一帧上传 */
    Uint32 fpsTick = SDL_GetTicks();
    Uint64 fpsFrames = 0;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_DROPFILE && ev.drop.file) {
                dropped.emplace_back(ev.drop.file);
                SDL_free(ev.drop.file);
                continue;   /* 不交给 ImGui,避免重复处理 */
            }
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window)) running = false;
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                g_vwin && ev.window.windowID == SDL_GetWindowID(g_vwin))
                closeVideoWindow();
        }

        /* 每秒统计一次解码帧率 */
        {
            const Uint32 now = SDL_GetTicks();
            if (now - fpsTick >= 1000) {
                const Uint64 frames = g_decoder.framesDecoded();
                g_fps = (float)(frames - fpsFrames) * 1000.0f / (float)(now - fpsTick);
                fpsFrames = frames;
                fpsTick = now;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        /* 拖入的文件:NRO 页作为待推送文件,其它页上传到文件页当前目录 */
        if (!dropped.empty()) {
            if (g_page == 2) {
                const std::string& p = dropped.front();
                snprintf(g_nxNroPath, sizeof(g_nxNroPath), "%s", p.c_str());
                if (endsWithNro(p) && strcmp(g_nxSdPath, "switch/") == 0) {
                    const char* base = strrchr(p.c_str(), '/');
                    base = base ? base + 1 : p.c_str();
                    snprintf(g_nxSdPath, sizeof(g_nxSdPath), "switch/%s", base);
                }
                setNxStatus("已选择文件,点\"推送到 SD 卡\"");
            } else if (!g_files) {
                setError("请先连接 Switch,再拖入文件上传");
            } else if (g_ftBusy) {
                setError("有传输任务进行中,请稍后再拖入文件");
            } else {
                beginUploadPaths(dropped);
                g_page = 1;
            }
            dropped.clear();
        }

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("SwLink", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
            ImGuiWindowFlags_NoSavedSettings);

        drawTopBar();

        /* 侧边栏 */
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##sidebar", ImVec2(196, 0), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        drawSidebar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::SameLine();

        /* 内容区 */
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 18));
        ImGui::BeginChild("##content", ImVec2(0, 0), false);
        if (g_page == 0)
            drawStreamPage(renderer);
        else if (g_page == 1)
            drawFilePage();
        else if (g_page == 2)
            drawNxlinkPage();
        else
            drawHelpPage();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::End();

        ImGui::Render();
        g_pixelScale = ImGui::GetIO().DisplayFramebufferScale.x;
        SDL_RenderSetScale(renderer, g_pixelScale, g_pixelScale);
        SDL_SetRenderDrawColor(renderer, 14, 16, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);

        /* 独立窗口每帧跟随更新 */
        drawVideoWindow();
    }

    disconnectDevice();

    closeVideoWindow();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
