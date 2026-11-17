// Sound for the PC build: one waveOut stream (44.1 kHz stereo) fed by a thread that mixes
//   - the game's own PCM sound, pulled from the engine's SoundPlayer.generateAudio (what Android's AudioTrack
//     callbacks do), and
//   - the 16 MediaPlayer music channels: MP3s (byte ranges inside the APK; a WAV works too) decoded with
//     Media Foundation.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <sys/system_properties.h>

#include "common.h"
#include "platform.h"

void (*g_music_stopped)(int ch) = nullptr;

namespace {

constexpr int OUT_RATE = 44100;
constexpr int MAX_BUF = 32;
// 8 buffers of 512 frames: 93 ms queued. The old 4 (46 ms) crackled on a desktop whose sound device (a USB headset
// with THX Spatial Audio) takes sound from waveOut in gulps up to ~50 ms apart: measured, only 11.6 ms was left
// ahead of the speaker at worst, so any slow fill() (the game's mixer, an MP3 decode) made a gap. With 8 buffers
// the worst was 57 ms, even with a 25 ms stall every second. Both can be changed in boz64.props
// (debug.boz64.audio_buffers, debug.boz64.audio_period).
int g_nbuf = 8;
int g_period = 512;  // frames per buffer

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

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
    IMFSourceReader* rd = nullptr;
    int rate = OUT_RATE, channels = 2;
    int repeat_left = 0;
    bool loop = false;
    float gain = 1.0f;
    bool eos = false;
    bool resume_after_suspend = false;
    s64 played = 0;     // source frames since the start of the track
    s64 duration = 0;   // 100 ns units
    Resampler rs;
};
std::mutex mlock;
Chan chans[MUSIC_CHANNELS];
bool mf_ok = false;

// ---- output ----
HWAVEOUT wo = nullptr;
HANDLE wo_event = nullptr;
WAVEHDR hdr[MAX_BUF];
std::vector<int16_t> bufs[MAX_BUF];
bool queued[MAX_BUF];
u64 written = 0;  // frames handed to waveOut

// Reads decoded PCM into the channel's FIFO until it holds `want` more frames or the track ends.
// Returns false on a decoding error.
bool decode(Chan& c, size_t want) {
    size_t target = c.rs.frames() + want;
    while (c.rs.frames() < target && !c.eos) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        HRESULT hr = c.rd->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, &ts, &s);
        if (FAILED(hr)) {
            release(s);
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            c.eos = true;
            release(s);
            break;
        }
        if (!s) continue;
        IMFMediaBuffer* b = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                auto* pcm = reinterpret_cast<const int16_t*>(p);
                size_t n = len / 2 / c.channels;
                for (size_t i = 0; i < n; i++) {
                    float l = pcm[i * c.channels] / 32768.0f;
                    float r = c.channels > 1 ? pcm[i * c.channels + 1] / 32768.0f : l;
                    c.rs.src.push_back(l);
                    c.rs.src.push_back(r);
                }
                b->Unlock();
            }
            release(b);
        }
        release(s);
    }
    return true;
}

void seek(Chan& c, s64 hns) {
    PROPVARIANT v;
    PropVariantInit(&v);
    v.vt = VT_I8;
    v.hVal.QuadPart = hns;
    c.rd->SetCurrentPosition(GUID_NULL, v);
    c.rs.clear();
    c.eos = false;
    c.played = hns * c.rate / 10000000;
}

void close_chan(Chan& c) {
    release(c.rd);
    c.rs.clear();
    c.eos = false;
    c.played = 0;
}

// Mixes one channel into out (n frames). Returns true if it stopped by itself (notify the game).
bool mix_chan(Chan& c, float* out, int n) {
    if (c.state != STARTED || !c.rd) return false;
    int done = 0;
    while (done < n) {
        if (!decode(c, c.rs.need(n - done))) {
            LOGW("music: decoding failed");
            c.state = ERRORED;
            close_chan(c);
            return true;
        }
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

double now_ms() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / freq;
}

// Sound handed to waveOut that the device has not played yet, in ms (-1 if unknown). At 0 the speaker plays a gap:
// that is the crackle. (The position counters are 32-bit, so the differences are taken modulo 2^32.)
double ahead_ms() {
    MMTIME t{};
    t.wType = TIME_SAMPLES;
    if (waveOutGetPosition(wo, &t, sizeof t) != MMSYSERR_NOERROR) return -1;
    u32 frames;
    if (t.wType == TIME_SAMPLES)
        frames = static_cast<u32>(written) - t.u.sample;
    else if (t.wType == TIME_BYTES)
        frames = (static_cast<u32>(written * 4) - t.u.cb) / 4;
    else
        return -1;
    if (frames > static_cast<u32>(MAX_BUF * g_period)) return 0;  // played past what was written: ran dry
    return frames * 1000.0 / OUT_RATE;
}

DWORD WINAPI feeder(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    // Every 10 s, if the sound came close to running out, the log says how close and how long the slowest fill()
    // took (a slow fill points at the game's mixer or music decoding; fast fills point at the sound device).
    const double buf_ms = g_period * 1000.0 / OUT_RATE;
    double report_at = now_ms() + 10000, lowest = 1e9, slowest = 0;
    int gaps = 0;
    bool primed = false;
    for (;;) {
        WaitForSingleObject(wo_event, 50);
        if (primed) {
            double a = ahead_ms();
            if (a >= 0) {
                lowest = std::min(lowest, a);
                if (a == 0) gaps++;
            }
        }
        for (int i = 0; i < g_nbuf; i++) {
            if (queued[i] && !(hdr[i].dwFlags & WHDR_DONE)) continue;
            double t = now_ms();
            fill(bufs[i].data());
            slowest = std::max(slowest, now_ms() - t);
            hdr[i].dwFlags &= ~WHDR_DONE;
            queued[i] = waveOutWrite(wo, &hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR;
            if (queued[i]) written += static_cast<u64>(g_period);
        }
        primed = true;
        if (now_ms() >= report_at) {
            if (gaps || lowest < buf_ms)
                LOGW("sound: ran out %d times in 10 s; at worst %.1f ms was left to play; slowest buffer fill %.1f ms "
                     "(%d buffers of %d frames: more buffers in boz64.props give more room)",
                     gaps, lowest < 1e9 ? lowest : 0.0, slowest, g_nbuf, g_period);
            report_at = now_ms() + 10000;
            lowest = 1e9;
            slowest = 0;
            gaps = 0;
        }
    }
}

// Reads [offset, offset + size) of a file (the whole file when size <= 0).
int read_range(const std::string& path, int64_t offset, int64_t size, std::vector<BYTE>& out) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    FILE* f = _wfopen(w.c_str(), L"rb");
    if (!f) return -1;
    if (size <= 0) {
        _fseeki64(f, 0, SEEK_END);
        size = _ftelli64(f);
        offset = 0;
    }
    out.resize(static_cast<size_t>(size));
    _fseeki64(f, offset, SEEK_SET);
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    return got == out.size() ? 0 : -2;
}

IMFSourceReader* open_reader(const std::vector<BYTE>& bytes, int& rate, int& channels, s64& duration) {
    IStream* st = SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size()));
    if (!st) return nullptr;
    IMFByteStream* bs = nullptr;
    IMFSourceReader* rd = nullptr;
    if (SUCCEEDED(MFCreateMFByteStreamOnStream(st, &bs))) {
        IMFAttributes* ba = nullptr;
        // the game's music is MP3 (Mule Kick's sting too, tools/mulekick_hud.py); a RIFF WAVE file also plays
        bool wav = bytes.size() > 12 && memcmp(bytes.data(), "RIFF", 4) == 0 && memcmp(bytes.data() + 8, "WAVE", 4) == 0;
        if (SUCCEEDED(bs->QueryInterface(IID_PPV_ARGS(&ba))))
            ba->SetString(MF_BYTESTREAM_CONTENT_TYPE, wav ? L"audio/wav" : L"audio/mpeg");
        release(ba);
        MFCreateSourceReaderFromByteStream(bs, nullptr, &rd);
    }
    release(bs);
    release(st);
    if (!rd) return nullptr;
    rd->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    rd->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
    IMFMediaType* mt = nullptr;
    MFCreateMediaType(&mt);
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    mt->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    mt->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    HRESULT hr = rd->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, mt);
    release(mt);
    IMFMediaType* cur = nullptr;
    if (FAILED(hr) || FAILED(rd->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &cur))) {
        release(rd);
        return nullptr;
    }
    rate = static_cast<int>(MFGetAttributeUINT32(cur, MF_MT_AUDIO_SAMPLES_PER_SECOND, OUT_RATE));
    channels = static_cast<int>(MFGetAttributeUINT32(cur, MF_MT_AUDIO_NUM_CHANNELS, 2));
    release(cur);
    PROPVARIANT v;
    PropVariantInit(&v);
    duration = 0;
    if (SUCCEEDED(rd->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v)))
        duration = static_cast<s64>(v.uhVal.QuadPart);
    PropVariantClear(&v);
    return rd;
}

bool valid(int ch) { return ch >= 0 && ch < MUSIC_CHANNELS; }

}  // namespace

void audio_open() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    mf_ok = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!mf_ok) LOGE("Media Foundation is not available: no music");
    char v[92] = {};
    if (__system_property_get("debug.boz64.audio_period", v) > 0 && atoi(v) >= 64) g_period = std::min(atoi(v), 8192);
    if (__system_property_get("debug.boz64.audio_buffers", v) > 0 && atoi(v) >= 2) g_nbuf = std::min(atoi(v), MAX_BUF);
    WAVEFORMATEX fmt{};
    fmt.wFormatTag = WAVE_FORMAT_PCM;
    fmt.nChannels = 2;
    fmt.nSamplesPerSec = OUT_RATE;
    fmt.wBitsPerSample = 16;
    fmt.nBlockAlign = 4;
    fmt.nAvgBytesPerSec = OUT_RATE * 4;
    wo_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, reinterpret_cast<DWORD_PTR>(wo_event), 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        LOGE("no sound output device");
        wo = nullptr;
        return;
    }
    for (int i = 0; i < g_nbuf; i++) {
        bufs[i].assign(static_cast<size_t>(g_period) * 2, 0);
        hdr[i] = WAVEHDR{};
        hdr[i].lpData = reinterpret_cast<LPSTR>(bufs[i].data());
        hdr[i].dwBufferLength = static_cast<DWORD>(bufs[i].size() * 2);
        waveOutPrepareHeader(wo, &hdr[i], sizeof(WAVEHDR));
        queued[i] = false;
    }
    // The game is never muted. Old test builds could mute a copy, and some left the game's volume in Windows' volume
    // mixer at 0 (Windows keeps it per .exe), which silenced every later run: put a saved 0 back to full.
    DWORD vol = 0;
    if (waveOutGetVolume(wo, &vol) == MMSYSERR_NOERROR && vol == 0) {
        waveOutSetVolume(wo, 0xffffffff);
        LOGI("sound: the game's volume in Windows' volume mixer was 0; set back to full");
    }
    LOGI("sound output: %d Hz stereo, %d buffers of %d frames (%.0f ms)", OUT_RATE, g_nbuf, g_period,
         g_nbuf * g_period * 1000.0 / OUT_RATE);
    CloseHandle(CreateThread(nullptr, 1 << 20, feeder, nullptr, 0, nullptr));
}

int audio_sfx_init(int volume, bool stereo, int rate) {
    sfx_rate = rate > 0 ? rate : OUT_RATE;
    sfx_channels = stereo ? 2 : 1;
    sfx_gain = std::clamp(volume, 0, 100) / 100.0f;
    return wo ? sfx_rate.load() : 0;
}
void audio_sfx_start() { sfx_on = true; }
void audio_sfx_stop() { sfx_on = false; }
void audio_sfx_volume(int volume) { sfx_gain = std::clamp(volume, 0, 100) / 100.0f; }
void audio_suspend() { suspended = true; }
void audio_resume() { suspended = false; }

int music_play(const std::string& path, int repeat, int64_t offset, int64_t size, int ch) {
    if (!valid(ch)) return -2;
    music_stop(ch);
    if (!mf_ok) return -2;
    std::vector<BYTE> bytes;
    int r = read_range(path, offset, size, bytes);
    if (r != 0) {
        LOGW("music: cannot read %s", path.c_str());
        return r;
    }
    int rate = OUT_RATE, channels = 2;
    s64 duration = 0;
    IMFSourceReader* rd = open_reader(bytes, rate, channels, duration);
    if (!rd) {
        LOGW("music: cannot decode %s (offset %lld)", path.c_str(), static_cast<long long>(offset));
        return -2;
    }
    IMFSourceReader* old;
    {
        std::lock_guard lk(mlock);
        Chan& c = chans[ch];
        old = c.rd;
        c.rd = rd;
        c.rate = rate;
        c.channels = std::max(1, channels);
        c.rs.clear();
        c.rs.step = static_cast<double>(rate) / OUT_RATE;
        c.loop = repeat == 0;
        c.repeat_left = repeat;
        c.eos = false;
        c.played = 0;
        c.duration = duration;
        c.state = STARTED;
    }
    release(old);
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
    return c.rd ? static_cast<int>(c.played * 1000 / std::max(1, c.rate)) : 0;
}

int music_duration_ms(int ch) {
    if (!valid(ch)) return 0;
    std::lock_guard lk(mlock);
    return chans[ch].rd ? static_cast<int>(chans[ch].duration / 10000) : 0;
}

void music_set_position_ms(int ch, int ms) {
    if (!valid(ch)) return;
    std::lock_guard lk(mlock);
    if (chans[ch].rd) seek(chans[ch], static_cast<s64>(ms) * 10000);
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
