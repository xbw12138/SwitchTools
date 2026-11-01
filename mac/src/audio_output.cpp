/*
 * audio_output.cpp
 */
#include "audio_output.hpp"

bool AudioOutput::init()
{
    SDL_AudioSpec want = {}, got = {};
    want.freq = kSampleRate;
    want.format = AUDIO_S16LSB;
    want.channels = kChannels;
    want.samples = 1024;
    want.callback = nullptr;   /* 用 SDL_QueueAudio 模式 */

    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0);
    if (dev_ == 0) return false;

    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void AudioOutput::shutdown()
{
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
}

void AudioOutput::queue(const uint8_t* data, size_t len)
{
    if (!dev_ || muted_) return;

    /* 积压超 0.5s 时丢旧数据,优先保延迟 */
    Uint32 queued = SDL_GetQueuedAudioSize(dev_);
    if (queued > kMaxBacklog)
        SDL_ClearQueuedAudio(dev_);

    SDL_QueueAudio(dev_, data, (Uint32)len);
}

void AudioOutput::setMuted(bool muted)
{
    muted_ = muted;
    if (!dev_) return;

    if (muted) {
        SDL_ClearQueuedAudio(dev_);
        SDL_PauseAudioDevice(dev_, 1);
    } else {
        SDL_PauseAudioDevice(dev_, 0);
    }
}
