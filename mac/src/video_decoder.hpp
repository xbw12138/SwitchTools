/*
 * video_decoder.hpp
 * FFmpeg H.264 解码:feed NAL -> 解码 -> 持最新 YUV420P 帧
 * UI 线程每帧调 uploadIfDirty() 把新帧灌进 SDL 纹理
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
}

class VideoDecoder {
public:
    bool init();
    void shutdown();

    /* 流线程调用:喂一个 Annex-B NAL(可含 SPS/PPS 前缀) */
    void feed(const uint8_t* data, size_t len);

    /* UI 线程调用:有新帧则更新纹理;返回可渲染纹理(可能为 nullptr) */
    SDL_Texture* uploadIfDirty(SDL_Renderer* renderer);

    /* 独立窗口调用:把最新 YUV 帧灌进外部纹理(纹理须为 IYUV/STREAMING) */
    bool uploadInto(SDL_Texture* tex);

    int width() const { return w_; }
    int height() const { return h_; }
    uint64_t framesDecoded() const { return frames_; }

private:
    const AVCodec* codec_ = nullptr;
    AVCodecParserContext* parser_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;

    std::mutex mtx_;
    std::vector<uint8_t> yuv_;   /* Y|U|V 紧凑存放 */
    int w_ = 0, h_ = 0;
    bool dirty_ = false;
    uint64_t frames_ = 0;

    SDL_Texture* tex_ = nullptr;
};
