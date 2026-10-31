/*
 * video_decoder.cpp
 */
#include "video_decoder.hpp"
#include <cstring>

bool VideoDecoder::init()
{
    codec_ = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec_) return false;

    parser_ = av_parser_init(AV_CODEC_ID_H264);
    if (!parser_) return false;

    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_) return false;

    /* 低延迟:不缓存帧重排(grc 输出不含 B 帧) */
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(ctx_, codec_, nullptr) < 0) return false;

    frame_ = av_frame_alloc();
    pkt_ = av_packet_alloc();
    return frame_ && pkt_;
}

void VideoDecoder::shutdown()
{
    if (parser_) { av_parser_close(parser_); parser_ = nullptr; }
    if (ctx_) { avcodec_free_context(&ctx_); ctx_ = nullptr; }
    if (frame_) { av_frame_free(&frame_); }
    if (pkt_) { av_packet_free(&pkt_); }
    if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; }
    w_ = h_ = 0;
}

void VideoDecoder::feed(const uint8_t* data, size_t len)
{
    if (!ctx_) return;

    while (len > 0) {
        uint8_t* out = nullptr;
        int outLen = 0;
        int used = av_parser_parse2(parser_, ctx_, &out, &outLen, data, (int)len,
                                    AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        data += used;
        len -= used;

        if (outLen > 0) {
            av_packet_unref(pkt_);
            pkt_->data = out;
            pkt_->size = outLen;

            if (avcodec_send_packet(ctx_, pkt_) == 0) {
                while (avcodec_receive_frame(ctx_, frame_) == 0) {
                    if (frame_->format == AV_PIX_FMT_YUV420P) {
                        std::lock_guard<std::mutex> lk(mtx_);
                        w_ = frame_->width;
                        h_ = frame_->height;
                        yuv_.resize((size_t)w_ * h_ * 3 / 2);
                        uint8_t* dst = yuv_.data();
                        for (int p = 0; p < 3; p++) {
                            int pw = (p == 0) ? w_ : w_ / 2;
                            int ph = (p == 0) ? h_ : h_ / 2;
                            for (int row = 0; row < ph; row++) {
                                memcpy(dst, frame_->data[p] + row * frame_->linesize[p], pw);
                                dst += pw;
                            }
                        }
                        dirty_ = true;
                        frames_++;
                    }
                }
            }
        }
    }
}

SDL_Texture* VideoDecoder::uploadIfDirty(SDL_Renderer* renderer)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (!dirty_) return tex_;
    dirty_ = false;

    if (!tex_ || w_ == 0 || h_ == 0) {
        if (tex_) { SDL_DestroyTexture(tex_); tex_ = nullptr; }
        tex_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                 SDL_TEXTUREACCESS_STREAMING, w_, h_);
        if (!tex_) return nullptr;
    }

    const uint8_t* y = yuv_.data();
    const uint8_t* u = y + (size_t)w_ * h_;
    const uint8_t* v = u + (size_t)w_ * h_ / 4;
    SDL_UpdateYUVTexture(tex_, nullptr, y, w_, u, w_ / 2, v, w_ / 2);
    return tex_;
}
