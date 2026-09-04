// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sound out.
//
// The game's audio microcode produces 16-bit stereo frames at whatever rate
// the game asked the AI for -- usually 32kHz, sometimes 22kHz, and it can
// change mid-game. SDL's audio stream resamples that to whatever the device
// wants, which is the whole job.
//
// ultramodern asks how many frames are still queued before it decides whether
// to run the audio thread again, so the one number that has to be right is
// get_frames_remaining. Report too few and the game overruns the buffer;
// report too many and it starves.

#include "host.hpp"

#include <SDL.h>

#include <cstdio>
#include <mutex>

namespace n64b {
namespace {

constexpr int kChannels = 2;
constexpr int kBytesPerFrame = kChannels * int(sizeof(int16_t));

SDL_AudioDeviceID device = 0;
SDL_AudioStream *stream = nullptr;
std::mutex stream_mutex;

int device_frequency = 48000;
int game_frequency = 32000;

/// Pulls whatever the resampler has ready into SDL's own buffer.
void audio_callback(void *userdata, uint8_t *out, int length) {
    std::lock_guard<std::mutex> lock(stream_mutex);
    int written = 0;
    if (stream != nullptr) {
        written = SDL_AudioStreamGet(stream, out, length);
        if (written < 0) {
            written = 0;
        }
    }
    // Silence rather than the previous buffer's contents, which is a click.
    if (written < length) {
        SDL_memset(out + written, 0, size_t(length - written));
    }
}

/// ultramodern counts what it hands over in samples -- one 16-bit value, half
/// of a stereo frame -- and asks for what is left in frames. Getting that the
/// wrong way round puts twice as much audio in as comes out, so the buffer
/// only ever grows, and a game that waits for its audio to drain before
/// building the next frame waits forever. Mario Builder 64 stopped after
/// nineteen of them.
void queue_samples(int16_t *samples, size_t sample_count) {
    std::lock_guard<std::mutex> lock(stream_mutex);
    if (stream == nullptr) {
        return;
    }
    SDL_AudioStreamPut(stream, samples, int(sample_count * sizeof(int16_t)));
}

size_t frames_remaining() {
    std::lock_guard<std::mutex> lock(stream_mutex);
    if (stream == nullptr) {
        return 0;
    }
    // What SDL still holds is in device frames; the game thinks in its own
    // rate, so convert back or a rate mismatch shows up as stutter.
    const int queued_bytes = SDL_AudioStreamAvailable(stream);
    const double device_frames = double(queued_bytes) / double(kBytesPerFrame);
    return size_t(device_frames * double(game_frequency) / double(device_frequency));
}

void set_frequency(uint32_t frequency) {
    std::lock_guard<std::mutex> lock(stream_mutex);
    if (int(frequency) == game_frequency && stream != nullptr) {
        return;
    }
    game_frequency = int(frequency);
    if (stream != nullptr) {
        SDL_FreeAudioStream(stream);
    }
    stream = SDL_NewAudioStream(AUDIO_S16SYS, kChannels, game_frequency,
                                AUDIO_S16SYS, kChannels, device_frequency);
    if (stream == nullptr) {
        std::fprintf(stderr, "could not build an audio resampler: %s\n", SDL_GetError());
    }
}

} // namespace

void init_audio() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "no audio: %s\n", SDL_GetError());
        return;
    }

    SDL_AudioSpec wanted{};
    wanted.freq = 48000;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = kChannels;
    // Small enough that the latency is not felt, large enough that the
    // callback is not the bottleneck.
    wanted.samples = 1024;
    wanted.callback = audio_callback;

    SDL_AudioSpec got{};
    device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &got, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (device == 0) {
        std::fprintf(stderr, "no audio device: %s\n", SDL_GetError());
        return;
    }

    device_frequency = got.freq;
    set_frequency(uint32_t(game_frequency));
    SDL_PauseAudioDevice(device, 0);
}

void shutdown_audio() {
    if (device != 0) {
        SDL_CloseAudioDevice(device);
        device = 0;
    }
    std::lock_guard<std::mutex> lock(stream_mutex);
    if (stream != nullptr) {
        SDL_FreeAudioStream(stream);
        stream = nullptr;
    }
}

ultramodern::audio_callbacks_t audio_callbacks() {
    return ultramodern::audio_callbacks_t{
        .queue_samples = queue_samples,
        .get_frames_remaining = frames_remaining,
        .set_frequency = set_frequency,
    };
}

} // namespace n64b
