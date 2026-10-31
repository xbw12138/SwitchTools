/*
 * main.cpp
 * SwLink macOS 客户端:SDL2 + ImGui 界面,串流 / 文件 两个页签
 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include "usb_link.hpp"
#include "stream_client.hpp"
#include "video_decoder.hpp"
#include "audio_output.hpp"
#include "file_client.hpp"
#include "../../protocol/swlink_protocol.h"

/* ---------------- 全局状态 ---------------- */
static UsbLink g_link;
static StreamClient g_stream(g_link);
static VideoDecoder g_decoder;
static AudioOutput g_audio;
static FileClient* g_files = nullptr;

static std::atomic<bool> g_connected{false};
static std::string g_status = "未连接";
static char g_errMsg[256] = {0};

static void setError(const std::string& s)
{
    snprintf(g_errMsg, sizeof(g_errMsg), "%s", s.c_str());
}

/* 连接 + 握手 + 启动串流线程 */
static bool connectDevice()
{
    if (!g_link.open()) { setError(g_link.lastError()); return false; }
    if (!g_link.handshake(true, true, 3)) {
        setError(g_link.lastError());
        g_link.close();
        return false;
    }

    if (!g_decoder.init()) { setError("解码器初始化失败"); g_link.close(); return false; }
    if (!g_audio.init())   { setError("音频设备初始化失败"); /* 不致命 */ }

    g_files = new FileClient(g_link);
    g_connected = true;
    g_status = "已连接,串流中";

    g_stream.start(
        [](const uint8_t* d, uint32_t l, uint8_t) { g_decoder.feed(d, l); },
        [](const uint8_t* d, uint32_t l) { g_audio.queue(d, l); },
        []() {
            g_connected = false;
            g_status = "连接已断开";
        });
    return true;
}

static void disconnectDevice()
{
    g_stream.stop();
    g_audio.shutdown();
    g_decoder.shutdown();
    delete g_files; g_files = nullptr;
    g_link.close();
    g_connected = false;
    g_status = "未连接";
}

/* ---------------- 串流页签 ---------------- */
static void drawStreamTab(SDL_Renderer* renderer)
{
    if (!g_connected) {
        if (ImGui::Button("连接 Switch")) {
            g_errMsg[0] = 0;
            connectDevice();
        }
    } else {
        if (ImGui::Button("断开")) disconnectDevice();
    }
    ImGui::SameLine();
    ImGui::Text("状态: %s", g_status.c_str());
    if (g_connected)
        ImGui::Text("已解码帧: %llu", (unsigned long long)g_decoder.framesDecoded());

    if (g_errMsg[0]) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.3f, 0.3f, 1));
        ImGui::TextWrapped("%s", g_errMsg);
        ImGui::PopStyleColor();
    }

    if (g_connected) {
        SDL_Texture* tex = g_decoder.uploadIfDirty(renderer);
        if (tex) {
            ImVec2 avail = ImGui::GetContentRegionAvail();
            float scale = avail.x / g_decoder.width();
            if (g_decoder.height() * scale > avail.y)
                scale = avail.y / g_decoder.height();
            ImGui::Image((ImTextureID)tex,
                         ImVec2(g_decoder.width() * scale, g_decoder.height() * scale));
        } else {
            ImGui::TextUnformatted("等待画面... (主菜单或不支持录制的游戏无输出)");
        }
    }
}

/* ---------------- 文件页签 ---------------- */
static char g_path[768] = "/";
static std::vector<FtEntry> g_entries;
static int g_selected = -1;
static std::string g_ftStatus;
static std::atomic<bool> g_ftBusy{false};
static float g_ftProgress = -1.f;   /* <0 表示无任务 */

static void refreshList()
{
    if (!g_files) return;
    g_entries.clear();
    g_selected = -1;
    if (!g_files->list(g_path, g_entries))
        setError(g_files->lastError());
}

static std::string joinPath(const char* dir, const std::string& name)
{
    std::string d = dir;
    if (d.back() != '/') d += '/';
    return d + name;
}

static void drawFileTab()
{
    if (!g_connected) {
        ImGui::TextUnformatted("请先连接设备");
        return;
    }

    ImGui::InputText("路径", g_path, sizeof(g_path));
    ImGui::SameLine();
    if (ImGui::Button("刷新") && !g_ftBusy) refreshList();

    /* 文件列表 */
    if (ImGui::BeginChild("list", ImVec2(0, -160), true)) {
        if (strcmp(g_path, "/") != 0) {
            if (ImGui::Selectable("../", false)) {
                std::string p = g_path;
                while (p.size() > 1 && p.back() == '/') p.pop_back();
                auto pos = p.find_last_of('/');
                p = (pos == 0 || pos == std::string::npos) ? "/" : p.substr(0, pos);
                snprintf(g_path, sizeof(g_path), "%s", p.c_str());
                refreshList();
            }
        }
        for (int i = 0; i < (int)g_entries.size(); i++) {
            auto& e = g_entries[i];
            char label[800];
            if (e.type == 1)
                snprintf(label, sizeof(label), "[D] %s", e.name.c_str());
            else
                snprintf(label, sizeof(label), "    %s (%.2f MB)", e.name.c_str(), e.size / 1048576.0);

            if (ImGui::Selectable(label, g_selected == i, ImGuiSelectableFlags_AllowDoubleClick)) {
                g_selected = i;
                if (ImGui::IsMouseDoubleClicked(0) && e.type == 1) {
                    snprintf(g_path, sizeof(g_path), "%s", joinPath(g_path, e.name).c_str());
                    refreshList();
                }
            }
        }
    }
    ImGui::EndChild();

    /* 操作按钮 */
    bool hasSel = g_selected >= 0 && g_selected < (int)g_entries.size();
    ImGui::BeginDisabled(g_ftBusy);

    if (ImGui::Button("下载") && hasSel && g_entries[g_selected].type == 2) {
        std::string remote = joinPath(g_path, g_entries[g_selected].name);
        g_ftBusy = true;
        std::thread([remote, name = g_entries[g_selected].name]() {
            g_ftProgress = 0;
            bool ok = g_files->readFile(remote, name,
                [](uint64_t d, uint64_t t) { g_ftProgress = t ? (float)d / t : 0; });
            g_ftStatus = ok ? "下载完成: " + name : "下载失败: " + g_files->lastError();
            g_ftProgress = -1;
            g_ftBusy = false;
        }).detach();
    }
    ImGui::SameLine();

    if (ImGui::Button("上传") && hasSel && g_entries[g_selected].type == 2) {
        /* 上传所选:把所选当本地文件上传到当前远程目录 */
        std::string localName = g_entries[g_selected].name;
        std::string remote = joinPath(g_path, localName);
        g_ftBusy = true;
        std::thread([localName, remote]() {
            g_ftProgress = 0;
            bool ok = g_files->writeFile(localName, remote,
                [](uint64_t d, uint64_t t) { g_ftProgress = t ? (float)d / t : 0; });
            g_ftStatus = ok ? "上传完成: " + localName : "上传失败: " + g_files->lastError();
            g_ftProgress = -1;
            g_ftBusy = false;
        }).detach();
    }
    ImGui::SameLine();

    if (ImGui::Button("删除") && hasSel) {
        std::string target = joinPath(g_path, g_entries[g_selected].name);
        if (g_files->remove(target, g_entries[g_selected].type == 1))
            refreshList();
        else setError(g_files->lastError());
    }
    ImGui::SameLine();

    static char newDir[256] = "";
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##newdir", "新目录名", newDir, sizeof(newDir));
    ImGui::SameLine();
    if (ImGui::Button("新建目录") && newDir[0]) {
        if (g_files->mkdir(joinPath(g_path, newDir))) { newDir[0] = 0; refreshList(); }
        else setError(g_files->lastError());
    }

    static char newName[256] = "";
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##rename", "重命名为", newName, sizeof(newName));
    ImGui::SameLine();
    if (ImGui::Button("重命名") && hasSel && newName[0]) {
        if (g_files->rename(joinPath(g_path, g_entries[g_selected].name), joinPath(g_path, newName))) {
            newName[0] = 0;
            refreshList();
        } else setError(g_files->lastError());
    }

    ImGui::EndDisabled();

    if (g_ftProgress >= 0)
        ImGui::ProgressBar(g_ftProgress, ImVec2(-1, 0));
    if (!g_ftStatus.empty())
        ImGui::TextUnformatted(g_ftStatus.c_str());
}

/* ---------------- main ---------------- */
int main(int, char**)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("SwLink",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!window || !renderer) {
        fprintf(stderr, "SDL 窗口/渲染器创建失败\n");
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL2_ProcessEvent(&ev);
            if (ev.type == SDL_QUIT) running = false;
            if (ev.type == SDL_WINDOWEVENT &&
                ev.window.event == SDL_WINDOWEVENT_CLOSE &&
                ev.window.windowID == SDL_GetWindowID(window)) running = false;
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("SwLink", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

        if (ImGui::BeginTabBar("main")) {
            if (ImGui::BeginTabItem("串流")) { drawStreamTab(renderer); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("文件")) { drawFileTab(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::End();

        ImGui::Render();
        SDL_RenderSetScale(renderer, ImGui::GetIO().DisplayFramebufferScale.x,
                                     ImGui::GetIO().DisplayFramebufferScale.y);
        SDL_SetRenderDrawColor(renderer, 30, 30, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    disconnectDevice();

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
