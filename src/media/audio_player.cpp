#include "media/audio_player.h"

#include "util/log.h"

#include <audioclient.h>
#include <initguid.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

// MinGW headers declare KSDATAFORMAT_SUBTYPE_IEEE_FLOAT via macros that
// reference an unresolved external. Define it ourselves so the linker
// can find it (this is the canonical Microsoft GUID). MSVC + the Windows
// SDK link the symbol from ksuser.lib, so we only do this on MinGW.
#ifdef __MINGW32__
namespace {
DEFINE_GUID(kKsFloatGuid,
    0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
}
#undef  KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
#define KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (kKsFloatGuid)
#endif

namespace volchay::media {
namespace {

// 60 ms WASAPI buffer (in 100-ns units). Large enough to absorb a
// missed wake-up of the worker thread without underrunning, small
// enough that A/V sync stays tight.
constexpr REFERENCE_TIME kBufferDuration = 600'000;   // 60 ms

LONGLONG us_to_mftime(core::TimeUs us) { return LONGLONG(us) * 10; }
core::TimeUs mftime_to_us(LONGLONG t)  { return core::TimeUs(t / 10); }

// MMDevice + WASAPI bundle owned by the worker thread.
struct AudioDevice {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice>           device;
    ComPtr<IAudioClient>        client;
    ComPtr<IAudioRenderClient>  render;
    ComPtr<IAudioClock>         clock;
    WAVEFORMATEX*               mix_format = nullptr;     // CoTaskMemFree
    UINT32                      buffer_frames = 0;
    HANDLE                      event = nullptr;          // ResetEvent / WaitForSingleObject

    ~AudioDevice() { reset(); }
    void reset() {
        if (event) { ::CloseHandle(event); event = nullptr; }
        if (mix_format) { ::CoTaskMemFree(mix_format); mix_format = nullptr; }
        clock.reset();
        render.reset();
        client.reset();
        device.reset();
        enumerator.reset();
        buffer_frames = 0;
    }
};

}  // namespace

AudioPlayer::AudioPlayer() {
    worker_ = std::thread(&AudioPlayer::worker_main, this);
}

AudioPlayer::~AudioPlayer() {
    quit_.store(true);
    {
        std::lock_guard lk(mu_);
        cv_.notify_all();
    }
    if (worker_.joinable()) worker_.join();
}

bool AudioPlayer::open(const std::wstring& path) {
    {
        std::lock_guard lk(mu_);
        pending_path_    = path;
        open_requested_  = true;
        close_requested_ = false;
    }
    cv_.notify_all();
    return true;        // open is async; check has_audio() after a frame
}

void AudioPlayer::close() {
    {
        std::lock_guard lk(mu_);
        close_requested_ = true;
        open_requested_  = false;
    }
    cv_.notify_all();
}

void AudioPlayer::play()             { playing_.store(true);  cv_.notify_all(); }
void AudioPlayer::pause()            { playing_.store(false); cv_.notify_all(); }
void AudioPlayer::seek(core::TimeUs t) {
    seek_target_.store(std::max<core::TimeUs>(0, t));
    cv_.notify_all();
}
void AudioPlayer::set_volume(float v) {
    volume_.store(std::clamp(v, 0.0f, 1.0f));
}
void AudioPlayer::set_muted(bool m) { muted_.store(m); }

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

namespace {

struct Worker {
    AudioPlayer*                self = nullptr;
    AudioDevice                 dev;
    ComPtr<IMFSourceReader>     reader;
    std::vector<unsigned char>  pcm;          // raw PCM for the next push.
    size_t                      pcm_offset = 0;
    UINT32                      sample_rate = 48000;
    UINT32                      channels    = 2;
    UINT32                      bits        = 32;       // float32 by default.
    bool                        is_float    = true;
    core::TimeUs                stream_pts_us = 0;
};

bool create_device(AudioDevice& d) {
    HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    __uuidof(IMMDeviceEnumerator),
                                    d.enumerator.put_void());
    if (FAILED(hr)) {
        log::err("Audio: CoCreateInstance(MMDeviceEnumerator) %08lx", long(hr));
        return false;
    }
    hr = d.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, d.device.put());
    if (FAILED(hr)) {
        log::err("Audio: GetDefaultAudioEndpoint %08lx", long(hr));
        return false;
    }
    hr = d.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            d.client.put_void());
    if (FAILED(hr)) {
        log::err("Audio: Activate(IAudioClient) %08lx", long(hr));
        return false;
    }
    hr = d.client->GetMixFormat(&d.mix_format);
    if (FAILED(hr)) {
        log::err("Audio: GetMixFormat %08lx", long(hr));
        return false;
    }
    hr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                              kBufferDuration, 0,
                              d.mix_format, nullptr);
    if (FAILED(hr)) {
        log::err("Audio: IAudioClient::Initialize %08lx", long(hr));
        return false;
    }
    d.event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    d.client->SetEventHandle(d.event);

    hr = d.client->GetBufferSize(&d.buffer_frames);
    if (FAILED(hr)) return false;
    hr = d.client->GetService(__uuidof(IAudioRenderClient),
                              d.render.put_void());
    if (FAILED(hr)) return false;
    hr = d.client->GetService(__uuidof(IAudioClock),
                              d.clock.put_void());
    if (FAILED(hr)) return false;
    return true;
}

bool open_audio_reader(Worker& w, const std::wstring& path) {
    ComPtr<IMFAttributes> attrs;
    if (FAILED(::MFCreateAttributes(attrs.put(), 1))) return false;
    HRESULT hr = ::MFCreateSourceReaderFromURL(path.c_str(), attrs.get(),
                                               w.reader.put());
    if (FAILED(hr)) {
        log::warn("Audio: MFCreateSourceReaderFromURL %08lx", long(hr));
        return false;
    }
    w.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    hr = w.reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    if (FAILED(hr)) {
        log::info("Audio: file has no audio stream");
        return false;
    }

    // Ask MF for matching PCM format.
    ComPtr<IMFMediaType> out_type;
    if (FAILED(::MFCreateMediaType(out_type.put()))) return false;
    out_type->SetGUID(MF_MT_MAJOR_TYPE,    MFMediaType_Audio);
    out_type->SetGUID(MF_MT_SUBTYPE,       MFAudioFormat_Float);
    out_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, w.sample_rate);
    out_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        w.channels);
    out_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     32);
    out_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,     w.channels * 4);
    out_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                        w.sample_rate * w.channels * 4);
    out_type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT,   TRUE);

    hr = w.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                       nullptr, out_type.get());
    if (FAILED(hr)) {
        log::warn("Audio: SetCurrentMediaType(Float32) %08lx", long(hr));
        return false;
    }
    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    if (SUCCEEDED(w.reader->GetPresentationAttribute(
            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) {
            ::PropVariantClear(&pv);
        }
    }
    ::PropVariantClear(&pv);
    return true;
}

void mix_into(BYTE* out, UINT32 frames, const float* src,
              UINT32 src_channels, UINT32 dst_channels,
              float gain) {
    auto* dst = reinterpret_cast<float*>(out);
    for (UINT32 f = 0; f < frames; ++f) {
        for (UINT32 c = 0; c < dst_channels; ++c) {
            float v = 0.0f;
            UINT32 sc = std::min(c, src_channels - 1);
            v = src[f * src_channels + sc] * gain;
            dst[f * dst_channels + c] = v;
        }
    }
}

}  // namespace

void AudioPlayer::worker_main() {
    HRESULT init = ::CoInitializeEx(nullptr,
                                    COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
    Worker w;
    w.self = this;

    while (!quit_.load()) {
        bool do_open  = false;
        bool do_close = false;
        std::wstring path;
        {
            std::unique_lock lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(20), [&] {
                return quit_.load() || open_requested_ || close_requested_;
            });
            if (open_requested_)  { do_open = true; path = pending_path_; }
            if (close_requested_) { do_close = true; }
            open_requested_ = close_requested_ = false;
        }

        if (do_close) {
            w.reader.reset();
            w.dev.reset();
            has_audio_.store(false);
            current_pts_us_.store(-1);
            duration_us_.store(0);
        }

        if (do_open) {
            // Reset previous state.
            w.reader.reset();
            w.dev.reset();
            w.pcm.clear();
            w.pcm_offset = 0;

            if (!create_device(w.dev)) {
                has_audio_.store(false);
                continue;
            }
            // Read mix format params back so we know what to push.
            if (w.dev.mix_format) {
                w.sample_rate = w.dev.mix_format->nSamplesPerSec;
                w.channels    = w.dev.mix_format->nChannels;
                if (w.dev.mix_format->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(
                        w.dev.mix_format);
                    w.is_float = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                } else {
                    w.is_float = (w.dev.mix_format->wFormatTag ==
                                  WAVE_FORMAT_IEEE_FLOAT);
                }
            }
            if (!w.is_float) {
                log::warn("Audio: device mix format is not float; skipping");
                w.dev.reset();
                has_audio_.store(false);
                continue;
            }
            if (!open_audio_reader(w, path)) {
                w.dev.reset();
                has_audio_.store(false);
                continue;
            }
            has_audio_.store(true);
            current_pts_us_.store(0);
            seek_target_.store(0);
            log::info("Audio engine ready: %u Hz, %u ch", w.sample_rate, w.channels);
        }

        if (!has_audio_.load()) continue;

        // Apply pending seek.
        core::TimeUs seek = seek_target_.exchange(-1);
        if (seek >= 0 && w.reader) {
            PROPVARIANT pv;
            ::PropVariantInit(&pv);
            pv.vt = VT_I8;
            pv.hVal.QuadPart = us_to_mftime(seek);
            w.reader->SetCurrentPosition(GUID_NULL, pv);
            ::PropVariantClear(&pv);
            w.pcm.clear();
            w.pcm_offset = 0;
            w.stream_pts_us = seek;
            current_pts_us_.store(seek);
        }

        if (!playing_.load()) {
            // Stop the WASAPI stream so other apps aren't blocked. The
            // device-side ring buffer is dropped, which gives an instant
            // pause (no trailing samples after the user clicks Pause).
            w.dev.client->Stop();
            w.dev.client->Reset();
            continue;
        }
        // Start the stream lazily.
        w.dev.client->Start();

        // Block on the buffer event up to ~50 ms. WASAPI in event-driven
        // shared mode signals this whenever there's room for more audio,
        // typically every 10–20 ms.
        ::WaitForSingleObject(w.dev.event, 50);

        UINT32 padding = 0;
        if (FAILED(w.dev.client->GetCurrentPadding(&padding))) continue;
        UINT32 free_frames = w.dev.buffer_frames - padding;
        if (free_frames == 0) continue;

        BYTE* out = nullptr;
        if (FAILED(w.dev.render->GetBuffer(free_frames, &out))) continue;

        UINT32 want_bytes = free_frames * w.channels * sizeof(float);
        UINT32 produced   = 0;

        while (produced < want_bytes) {
            // Refill PCM cache if exhausted.
            if (w.pcm_offset >= w.pcm.size()) {
                w.pcm.clear();
                w.pcm_offset = 0;

                DWORD       stream_index = 0;
                DWORD       flags        = 0;
                LONGLONG    ts           = 0;
                ComPtr<IMFSample> sample;
                HRESULT hr = w.reader->ReadSample(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                    &stream_index, &flags, &ts, sample.put());
                if (FAILED(hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                    || !sample) {
                    // End of audio: zero-fill the rest.
                    std::memset(out + produced, 0, want_bytes - produced);
                    produced = want_bytes;
                    break;
                }
                w.stream_pts_us = mftime_to_us(ts);

                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())))
                    continue;
                BYTE* data = nullptr;
                DWORD max_len = 0, cur_len = 0;
                if (FAILED(buffer->Lock(&data, &max_len, &cur_len))) continue;
                w.pcm.assign(data, data + cur_len);
                buffer->Unlock();
            }

            UINT32 take = std::min<UINT32>(want_bytes - produced,
                                           UINT32(w.pcm.size() - w.pcm_offset));
            if (muted_.load()) {
                std::memset(out + produced, 0, take);
            } else {
                const float gain = volume_.load();
                if (gain >= 0.999f) {
                    std::memcpy(out + produced, &w.pcm[w.pcm_offset], take);
                } else {
                    auto* src = reinterpret_cast<const float*>(
                        &w.pcm[w.pcm_offset]);
                    auto* dst = reinterpret_cast<float*>(out + produced);
                    UINT32 n_floats = take / sizeof(float);
                    for (UINT32 i = 0; i < n_floats; ++i) dst[i] = src[i] * gain;
                }
            }
            w.pcm_offset += take;
            produced     += take;
        }

        UINT32 frames_written = want_bytes / (w.channels * sizeof(float));
        w.dev.render->ReleaseBuffer(frames_written, 0);

        // Update master clock from IAudioClock.
        UINT64 pos = 0, freq = 0;
        if (SUCCEEDED(w.dev.clock->GetFrequency(&freq))
            && SUCCEEDED(w.dev.clock->GetPosition(&pos, nullptr))
            && freq > 0) {
            core::TimeUs played_us = core::TimeUs(double(pos) * 1'000'000.0 /
                                                  double(freq));
            current_pts_us_.store(played_us);
        } else {
            current_pts_us_.store(w.stream_pts_us);
        }
    }

    if (w.dev.client) w.dev.client->Stop();
    w.reader.reset();
    w.dev.reset();
    if (SUCCEEDED(init)) ::CoUninitialize();
}

}  // namespace volchay::media
