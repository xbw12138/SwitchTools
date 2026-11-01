/*
 * audio_output.hpp
 * SDL 音频队列播放:PCM s16le 48kHz 立体声;积压超阈值丢旧数据
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <SDL.h>

class AudioOutput {
public:
    bool init();
    void shutdown();

    /* 流线程调用 */
    void queue(const uint8_t* data, size_t len);

    /* UI 线程调用:静音/取消静音(静音时暂停设备并清空积压) */
    void setMuted(bool muted);
    bool muted() const { return muted_; }

    /* 设备侧 0x1000 字节/块 */
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;

private:
    SDL_AudioDeviceID dev_ = 0;
    bool muted_ = false;
    /* 最大积压 ~0.5s: 48000 * 2ch * 2B * 0.5 */
    static constexpr size_t kMaxBacklog = 48000 * 2 * 2 / 2;
};
