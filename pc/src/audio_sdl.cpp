// Sound for the Unix PC builds, through SDL3 (audio_win.cpp is the Windows version; the mixing is the same): one
// 44.1 kHz stereo stream that SDL pulls from, filled with
//   - the game's own PCM sound, pulled from the engine's SoundPlayer.generateAudio (what Android's AudioTrack
//     callbacks do), and
//   - the 16 MediaPlayer music channels: MP3s (byte ranges inside the APK; a WAV works too), decoded with dr_mp3 and
//     dr_wav (pc/third_party/dr_libs).
// The game is never muted: nothing here sets or saves a volume outside the game's own.
#include <SDL3/SDL.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <sys/system_properties.h>

#include "common.h"
#include "platform.h"

void (*g_music_stopped)(int ch) = nullptr;

namespace {

constexpr int OUT_RATE = 44100;
int g_period = 512;  // frames mixed at a time (debug.boz64.audio_period); SDL is asked for buffers this size

// Linear-interpolating rate converter over a FIFO of stereo float frames.
struct Resampler {
    double step = 1.0;  // source frames per output frame
    double pos = 0.0;
    std::vector<float> src;  // interleaved L, R
    size_t frames() const { return src.size() / 2; }
    // frames still needed in the FIFO to produce n output frames
    size_t need(int n) const {
        size_t want = static_cast<size_t>(pos + n * step) + 2;
        return want > frames() ? want - frames() : 0;
    }
    // adds n output frames (times gain) into out; returns how many it could make
    int produce(float* out, int n, float gain) {
        int made = 0;
        for (; made < n; made++) {
            size_t i = static_cast<size_t>(pos);
            if (i + 1 >= frames()) break;
            float f = static_cast<float>(pos - i);
            out[2 * made] += gain * (src[2 * i] + f * (src[2 * i + 2] - src[2 * i]));
            out[2 * made + 1] += gain * (src[2 * i + 1] + f * (src[2 * i + 3] - src[2 * i + 1]));
            pos += step;
        }
        size_t used = std::min(static_cast<size_t>(pos), frames());
        src.erase(src.begin(), src.begin() + 2 * used);
        pos -= static_cast<double>(used);
        return made;
    }
    void clear() {
        src.clear();
        pos = 0;
    }
};

// One decoded track: an MP3 or a WAV held in memory.
struct Track {
    std::vector<unsigned char> bytes;  // the decoders read from here
    bool wav = false;
    drmp3 mp3{};
    drwav wv{};
    int rate = OUT_RATE, channels = 2;
    s64 frames = 0;  // length in source frames (0 if unknown)

    bool open() {
        wav = bytes.size() > 12 && memcmp(bytes.data(), "RIFF", 4) == 0 && memcmp(bytes.data() + 8, "WAVE", 4) == 0;
        if (wav) {
            if (!drwav_init_memory(&wv, bytes.data(), bytes.size(), nullptr)) return false;
            rate = static_cast<int>(wv.sampleRate);
            channels = wv.channels;
            frames = static_cast<s64>(wv.totalPCMFrameCount);
        } else {
            if (!drmp3_init_memory(&mp3, bytes.data(), bytes.size(), nullptr)) return false;
            rate = static_cast<int>(mp3.sampleRate);
            channels = static_cast<int>(mp3.channels);
            frames = static_cast<s64>(drmp3_get_pcm_frame_count(&mp3));
            drmp3_seek_to_pcm_frame(&mp3, 0);  // counting the frames read to the end
        }
        channels = std::max(1, channels);
        return rate > 0;
    }
    // reads up to n frames of 16-bit PCM; 0 at the end
    size_t read(int16_t* out, size_t n) {
        return static_cast<size_t>(wav ? drwav_read_pcm_frames_s16(&wv, n, out) : drmp3_read_pcm_frames_s16(&mp3, n, out));
    }
    void seek(s64 frame) {
        if (wav) drwav_seek_to_pcm_frame(&wv, static_cast<drwav_uint64>(frame));
        else drmp3_seek_to_pcm_frame(&mp3, static_cast<drmp3_uint64>(frame));
    }
    ~Track() {
        if (wav) drwav_uninit(&wv);
        else drmp3_uninit(&mp3);
    }
};

// ---- the game's PCM sound ----
std::atomic<int> sfx_rate{OUT_RATE}, sfx_channels{2};
std::atomic<float> sfx_gain{1.0f};
std::atomic<bool> sfx_on{false}, suspended{false};
Resampler sfx_rs;
std::vector<int16_t> sfx_tmp;

// ---- music ----
enum State { NONE = 0, STARTED = 5, PAUSED = 6, STOPPED = 3, ERRORED = 8 };
struct Chan {
    State state = NONE;
    std::unique_ptr<Track> tr;
    int repeat_left = 0;
    bool loop = false;
    float gain = 1.0f;
    bool eos = false;
    bool resume_after_suspend = false;
    s64 played = 0;  // source frames since the start of the track
    Resampler rs;
};
std::mutex mlock;
Chan chans[MUSIC_CHANNELS];

// ---- output ----
SDL_AudioStream* stream = nullptr;
std::vector<int16_t> out_buf;

// Reads decoded PCM into the channel's FIFO until it holds `want` more frames or the track ends.
void decode(Chan& c, size_t want) {
    size_t target = c.rs.frames() + want;
    int16_t pcm[4096];
    int ch = c.tr->channels;
    size_t per = sizeof pcm / sizeof pcm[0] / static_cast<size_t>(ch);
    while (c.rs.frames() < target && !c.eos) {
        size_t n = c.tr->read(pcm, std::min(per, target - c.rs.frames()));
        if (n == 0) {
            c.eos = true;
            break;
        }
        for (size_t i = 0; i < n; i++) {
            float l = pcm[i * ch] / 32768.0f;
            c.rs.src.push_back(l);
            c.rs.src.push_back(ch > 1 ? pcm[i * ch + 1] / 32768.0f : l);
        }
    }
}

void seek(Chan& c, s64 frame) {
    c.tr->seek(frame);
    c.rs.clear();
    c.eos = false;
    c.played = frame;
}

void close_chan(Chan& c) {
    c.tr.reset();
    c.rs.clear();
    c.eos = false;
    c.played = 0;
}

// Mixes one channel into out (n frames). Returns true if it stopped by itself (notify the game).
bool mix_chan(Chan& c, float* out, int n) {
    if (c.state != STARTED || !c.tr) return false;
    int done = 0;
    while (done < n) {
        decode(c, c.rs.need(n - done));
        size_t before = c.rs.frames();
        int made = c.rs.produce(out + 2 * done, n - done, c.gain);
        c.played += static_cast<s64>(before - c.rs.frames());
        done += made;
        if (done < n && c.eos) {
            // the track ended: loop, play again, or stop (MediaPlayer's completion -> audioStop -> audioStoppedNotify)
            if (c.loop || --c.repeat_left > 0) {
                seek(c, 0);
                continue;
            }
            c.state = STOPPED;
            close_chan(c);
            return true;
        }
        if (made == 0 && !c.eos) break;  // nothing decodable right now
    }
    return false;
}

void fill(int16_t* dst) {
    std::vector<float> mix(static_cast<size_t>(g_period) * 2, 0.0f);
    if (sfx_on && !suspended) {
        int ch = sfx_channels;
        sfx_rs.step = static_cast<double>(sfx_rate) / OUT_RATE;
        size_t need = sfx_rs.need(g_period);
        if (need) {
            sfx_tmp.resize(need * ch);
            if (marm_generate_audio(sfx_tmp.data(), static_cast<int>(need), ch)) {
                for (size_t i = 0; i < need; i++) {
                    float l = sfx_tmp[i * ch] / 32768.0f;
                    sfx_rs.src.push_back(l);
                    sfx_rs.src.push_back(ch > 1 ? sfx_tmp[i * ch + 1] / 32768.0f : l);
                }
            } else {
                sfx_rs.src.insert(sfx_rs.src.end(), need * 2, 0.0f);
            }
        }
        sfx_rs.produce(mix.data(), g_period, sfx_gain);
    }
    std::vector<int> stopped;
    {
        std::lock_guard lk(mlock);
        for (int i = 0; i < MUSIC_CHANNELS; i++)
            if (mix_chan(chans[i], mix.data(), g_period)) stopped.push_back(i);
    }
    for (size_t i = 0; i < mix.size(); i++) {
        float v = std::clamp(mix[i], -1.0f, 1.0f);
        dst[i] = static_cast<int16_t>(std::lrint(v * 32767.0f));
    }
    for (int ch : stopped)
        if (g_music_stopped) g_music_stopped(ch);
}

// SDL's audio thread wants `additional` more bytes: mix whole periods until it has them.
void SDLCALL feed(void*, SDL_AudioStream* s, int additional, int) {
    const int bytes = g_period * 4;
    while (additional > 0) {
        fill(out_buf.data());
        SDL_PutAudioStreamData(s, out_buf.data(), bytes);
        additional -= bytes;
    }
}

// Reads [offset, offset + size) of a file (the whole file when size <= 0).
int read_range(const std::string& path, int64_t offset, int64_t size, std::vector<unsigned char>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return -1;
    if (size <= 0) {
        fseeko(f, 0, SEEK_END);
        size = ftello(f);
        offset = 0;
    }
    out.resize(static_cast<size_t>(size));
    fseeko(f, offset, SEEK_SET);
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size() ? 0 : -2;
}

bool valid(int ch) { return ch >= 0 && ch < MUSIC_CHANNELS; }

}  // namespace

void audio_open() {
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.audio_period", v) > 0 && atoi(v) >= 64) g_period = std::min(atoi(v), 8192);
    out_buf.assign(static_cast<size_t>(g_period) * 2, 0);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, std::to_string(g_period).c_str());
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        LOGE("no sound: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec{SDL_AUDIO_S16, 2, OUT_RATE};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, nullptr);
    if (!stream) {
        LOGE("no sound output device: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream);
    LOGI("sound output: %d Hz stereo, %d-frame periods, through %s", OUT_RATE, g_period, SDL_GetCurrentAudioDriver());
}

int audio_sfx_init(int volume, bool stereo, int rate) {
    sfx_rate = rate > 0 ? rate : OUT_RATE;
    sfx_channels = stereo ? 2 : 1;
    sfx_gain = std::clamp(volume, 0, 100) / 100.0f;
    return stream ? sfx_rate.load() : 0;
}
void audio_sfx_start() { sfx_on = true; }
void audio_sfx_stop() { sfx_on = false; }
void audio_sfx_volume(int volume) { sfx_gain = std::clamp(volume, 0, 100) / 100.0f; }
void audio_suspend() { suspended = true; }
void audio_resume() { suspended = false; }

int music_play(const std::string& path, int repeat, int64_t offset, int64_t size, int ch) {
    if (!valid(ch)) return -2;
    music_stop(ch);
    auto tr = std::make_unique<Track>();
    int r = read_range(path, offset, size, tr->bytes);
    if (r != 0) {
        LOGW("music: cannot read %s", path.c_str());
        return r;
    }
    if (!tr->open()) {
        LOGW("music: cannot decode %s (offset %lld)", path.c_str(), static_cast<long long>(offset));
        return -2;
    }
    std::unique_ptr<Track> old;
    {
        std::lock_guard lk(mlock);
        Chan& c = chans[ch];
        old = std::move(c.tr);
        c.rs.clear();
        c.rs.step = static_cast<double>(tr->rate) / OUT_RATE;
        c.tr = std::move(tr);
        c.loop = repeat == 0;
        c.repeat_left = repeat;
        c.eos = false;
        c.played = 0;
        c.state = STARTED;
    }
    return 0;
}

void music_stop(int ch) {
    if (!valid(ch)) return;
    bool notify = false;
    {
        std::lock_guard lk(mlock);
        Chan& c = chans[ch];
        notify = c.state == STARTED || c.state == PAUSED;
        if (c.state != NONE) c.state = STOPPED;
        close_chan(c);
    }
    if (notify && g_music_stopped) g_music_stopped(ch);
}

int music_pause(int ch) {
    if (!valid(ch)) return -1;
    std::lock_guard lk(mlock);
    if (chans[ch].state != STARTED) return -1;
    chans[ch].state = PAUSED;
    return 0;
}

int music_resume(int ch) {
    if (!valid(ch)) return -1;
    std::lock_guard lk(mlock);
    if (chans[ch].state != PAUSED) return -1;
    chans[ch].state = STARTED;
    return 0;
}

int music_status(int ch) {
    if (!valid(ch)) return 0;
    std::lock_guard lk(mlock);
    switch (chans[ch].state) {
    case STARTED: return 1;
    case PAUSED: return 2;
    case ERRORED: return 3;
    default: return 0;
    }
}

int music_position_ms(int ch) {
    if (!valid(ch)) return 0;
    std::lock_guard lk(mlock);
    const Chan& c = chans[ch];
    return c.tr ? static_cast<int>(c.played * 1000 / std::max(1, c.tr->rate)) : 0;
}

int music_duration_ms(int ch) {
    if (!valid(ch)) return 0;
    std::lock_guard lk(mlock);
    const Chan& c = chans[ch];
    return c.tr ? static_cast<int>(c.tr->frames * 1000 / std::max(1, c.tr->rate)) : 0;
}

void music_set_position_ms(int ch, int ms) {
    if (!valid(ch)) return;
    std::lock_guard lk(mlock);
    Chan& c = chans[ch];
    if (c.tr) seek(c, static_cast<s64>(ms) * c.tr->rate / 1000);
}

void music_set_volume(int ch, int volume) {
    if (!valid(ch)) return;
    std::lock_guard lk(mlock);
    chans[ch].gain = std::clamp(volume, 0, 100) / 100.0f;
}

bool music_is_playing(int ch) {
    if (!valid(ch)) return false;
    std::lock_guard lk(mlock);
    return chans[ch].state == STARTED;
}

void music_stop_all() {
    for (int i = 0; i < MUSIC_CHANNELS; i++) {
        std::lock_guard lk(mlock);
        if (chans[i].state != NONE) chans[i].state = STOPPED;
        close_chan(chans[i]);
    }
}

void music_suspend() {
    std::lock_guard lk(mlock);
    for (Chan& c : chans) {
        c.resume_after_suspend = c.state == STARTED;
        if (c.resume_after_suspend) c.state = PAUSED;
    }
}

void music_resume_all() {
    std::lock_guard lk(mlock);
    for (Chan& c : chans)
        if (c.resume_after_suspend && c.state == PAUSED) c.state = STARTED;
}
