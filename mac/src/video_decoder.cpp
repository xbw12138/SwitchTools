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

    /* 切片多线程:提高解码吞吐,同时避免帧级线程带来的额外延迟 */
    ctx_->thread_count = 4;
    ctx_->thread_type = FF_THREAD_SLICE;

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
                        /* 逐平面一次拷贝走完(源为连续行,linesize 可能含填充,故按行) */
                        const int planeW[3] = { w_, w_ / 2, w_ / 2 };
                        const int planeH[3] = { h_, h_ / 2, h_ / 2 };
                        for (int p = 0; p < 3; p++) {
                            if (frame_->linesize[p] == planeW[p]) {
                                memcpy(dst, frame_->data[p],
                                       (size_t)planeW[p] * planeH[p]);
                                dst += (size_t)planeW[p] * planeH[p];
                            } else {
                                for (int row = 0; row < planeH[p]; row++) {
                                    memcpy(dst, frame_->data[p] + row * frame_->linesize[p], planeW[p]);
                                    dst += planeW[p];
                                }
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

bool VideoDecoder::uploadInto(SDL_Texture* tex)
{
    if (!tex) return false;

    std::lock_guard<std::mutex> lk(mtx_);
    if (w_ <= 0 || h_ <= 0 || yuv_.empty()) return false;

    const uint8_t* y = yuv_.data();
    const uint8_t* u = y + (size_t)w_ * h_;
    const uint8_t* v = u + (size_t)w_ * h_ / 4;
    SDL_UpdateYUVTexture(tex, nullptr, y, w_, u, w_ / 2, v, w_ / 2);
    return true;
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
        /* 线性缩放:窗口尺寸和 720p 不完全一致,最近邻会明显发虚/锯齿 */
        SDL_SetTextureScaleMode(tex_, SDL_ScaleModeLinear);
    }

    const uint8_t* y = yuv_.data();
    const uint8_t* u = y + (size_t)w_ * h_;
    const uint8_t* v = u + (size_t)w_ * h_ / 4;
    SDL_UpdateYUVTexture(tex_, nullptr, y, w_, u, w_ / 2, v, w_ / 2);
    return tex_;
}
