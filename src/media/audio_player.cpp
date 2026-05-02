// libavcodec + WASAPI audio renderer.
// (File name kept for source-compat; previously this used Media
// Foundation IMFSourceReader.)
#include "media/audio_player.h"

#include "util/log.h"

#include <audioclient.h>
#include <initguid.h>
#include <mmdeviceapi.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <algorithm>
#include <cstring>
#include <vector>

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

// 60 ms WASAPI buffer (in 100-ns units).
constexpr REFERENCE_TIME kBufferDuration = 600'000;

std::string averr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

std::string narrow_path(const std::wstring& w) {
    int sz = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(sz, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                          out.data(), sz, nullptr, nullptr);
    return out;
}

struct AudioDevice {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice>           device;
    ComPtr<IAudioClient>        client;
    ComPtr<IAudioRenderClient>  render;
    ComPtr<IAudioClock>         clock;
    WAVEFORMATEX*               mix_format = nullptr;
    UINT32                      buffer_frames = 0;
    HANDLE                      event = nullptr;

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

struct Worker {
    AudioPlayer*      self = nullptr;
    AudioDevice       dev;

    // libav decoder.
    AVFormatContext*  fmt_ctx     = nullptr;
    AVCodecContext*   codec_ctx   = nullptr;
    SwrContext*       swr         = nullptr;
    AVPacket*         packet      = nullptr;
    AVFrame*          frame       = nullptr;
    int               audio_stream= -1;
    int64_t           ts_offset   = 0;

    // Resampled PCM cache.
    std::vector<unsigned char>  pcm;
    size_t                      pcm_offset = 0;
    UINT32                      sample_rate = 48000;
    UINT32                      channels    = 2;
    bool                        is_float    = true;
    bool                        eof_reached = false;
    core::TimeUs                stream_pts_us = 0;

    // Master clock anchor.
    core::TimeUs                clock_origin_us  = 0;
    UINT64                      clock_origin_pos = 0;
    bool                        clock_dirty      = true;
    bool                        device_running   = false;
};

void release_decoder(Worker& w) {
    if (w.swr)       { swr_free(&w.swr);                            }
    if (w.frame)     { av_frame_free(&w.frame);                     }
    if (w.packet)    { av_packet_free(&w.packet);                   }
    if (w.codec_ctx) { avcodec_free_context(&w.codec_ctx);          }
    if (w.fmt_ctx)   { avformat_close_input(&w.fmt_ctx);            }
    w.audio_stream = -1;
    w.eof_reached  = false;
    w.pcm.clear();
    w.pcm_offset = 0;
}

bool open_audio_decoder(Worker& w, const std::wstring& path) {
    const std::string utf8 = narrow_path(path);

    int err = avformat_open_input(&w.fmt_ctx, utf8.c_str(), nullptr, nullptr);
    if (err < 0) {
        log::warn("Audio: avformat_open_input %s", averr(err).c_str());
        return false;
    }
    err = avformat_find_stream_info(w.fmt_ctx, nullptr);
    if (err < 0) {
        log::warn("Audio: avformat_find_stream_info %s", averr(err).c_str());
        return false;
    }
    int astream = av_find_best_stream(w.fmt_ctx, AVMEDIA_TYPE_AUDIO,
                                      -1, -1, nullptr, 0);
    if (astream < 0) {
        log::info("Audio: file has no audio stream");
        return false;
    }
    w.audio_stream = astream;
    AVStream* st = w.fmt_ctx->streams[astream];

    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        log::warn("Audio: no decoder for codec %d", int(st->codecpar->codec_id));
        return false;
    }
    w.codec_ctx = avcodec_alloc_context3(dec);
    if (!w.codec_ctx) return false;
    err = avcodec_parameters_to_context(w.codec_ctx, st->codecpar);
    if (err < 0) {
        log::warn("Audio: parameters_to_context %s", averr(err).c_str());
        return false;
    }
    err = avcodec_open2(w.codec_ctx, dec, nullptr);
    if (err < 0) {
        log::warn("Audio: avcodec_open2 %s", averr(err).c_str());
        return false;
    }

    w.packet = av_packet_alloc();
    w.frame  = av_frame_alloc();
    if (!w.packet || !w.frame) return false;

    // Build resampler: input = decoder format, output = WASAPI mix
    // format (32-bit float, device's sample rate and channel count).
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, int(w.channels));

    AVChannelLayout in_layout;
    if (w.codec_ctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_copy(&in_layout, &w.codec_ctx->ch_layout);
    } else {
        av_channel_layout_default(&in_layout, w.codec_ctx->ch_layout.nb_channels);
    }

    SwrContext* swr = nullptr;
    err = swr_alloc_set_opts2(&swr,
        &out_layout, AV_SAMPLE_FMT_FLT, int(w.sample_rate),
        &in_layout,  w.codec_ctx->sample_fmt, w.codec_ctx->sample_rate,
        0, nullptr);
    if (err < 0 || !swr) {
        log::warn("Audio: swr_alloc_set_opts2 %s", averr(err).c_str());
        return false;
    }
    w.swr = swr;
    err = swr_init(w.swr);
    if (err < 0) {
        log::warn("Audio: swr_init %s", averr(err).c_str());
        return false;
    }
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    w.ts_offset = (st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;
    return true;
}

// Pull more PCM into w.pcm by reading & decoding packets, then
// resampling to the device's float format. Writes raw bytes (size
// matches w.channels * sizeof(float) per frame). Returns false on EOF
// (with whatever residual was flushed already).
bool refill_pcm(Worker& w) {
    if (!w.codec_ctx || !w.swr) return false;
    AVStream* st = w.fmt_ctx->streams[w.audio_stream];

    int packet_safety = 0;
    while (true) {
        if (++packet_safety > 1024) return false;

        // Drain decoded frames first.
        int err = avcodec_receive_frame(w.codec_ctx, w.frame);
        if (err == 0) {
            int64_t pts_ts = (w.frame->best_effort_timestamp != AV_NOPTS_VALUE)
                ? w.frame->best_effort_timestamp
                : w.frame->pts;
            if (pts_ts != AV_NOPTS_VALUE) {
                w.stream_pts_us = core::TimeUs(av_rescale_q(
                    pts_ts - w.ts_offset, st->time_base,
                    AVRational{1, 1'000'000}));
            }
            // Estimate output sample count, with a small safety margin.
            int out_samples = int(av_rescale_rnd(
                swr_get_delay(w.swr, w.codec_ctx->sample_rate)
                    + w.frame->nb_samples,
                int64_t(w.sample_rate),
                w.codec_ctx->sample_rate, AV_ROUND_UP));
            const size_t out_bytes =
                size_t(out_samples) * w.channels * sizeof(float);
            const size_t old_size = w.pcm.size();
            w.pcm.resize(old_size + out_bytes);

            uint8_t* out_ptr = w.pcm.data() + old_size;
            int got = swr_convert(w.swr,
                &out_ptr, out_samples,
                (const uint8_t**)w.frame->extended_data, w.frame->nb_samples);
            if (got < 0) {
                log::warn("Audio: swr_convert %s", averr(got).c_str());
                w.pcm.resize(old_size);
                av_frame_unref(w.frame);
                return false;
            }
            const size_t produced =
                size_t(got) * w.channels * sizeof(float);
            w.pcm.resize(old_size + produced);
            av_frame_unref(w.frame);
            return true;
        }
        if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
            log::warn("Audio: receive_frame %s", averr(err).c_str());
            return false;
        }
        if (err == AVERROR_EOF) {
            w.eof_reached = true;
            return false;
        }

        // Read another packet.
        err = av_read_frame(w.fmt_ctx, w.packet);
        if (err == AVERROR_EOF) {
            avcodec_send_packet(w.codec_ctx, nullptr);  // drain
            continue;
        }
        if (err < 0) {
            log::warn("Audio: av_read_frame %s", averr(err).c_str());
            return false;
        }
        if (w.packet->stream_index != w.audio_stream) {
            av_packet_unref(w.packet);
            continue;
        }
        err = avcodec_send_packet(w.codec_ctx, w.packet);
        av_packet_unref(w.packet);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            log::warn("Audio: send_packet %s", averr(err).c_str());
            return false;
        }
    }
}

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
    return true;
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
    const auto target = std::max<core::TimeUs>(0, t);
    seek_target_.store(target);
    current_pts_us_.store(target);
    cv_.notify_all();
}
void AudioPlayer::set_volume(float v) {
    volume_.store(std::clamp(v, 0.0f, 1.0f));
}
void AudioPlayer::set_muted(bool m) { muted_.store(m); }

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

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
            release_decoder(w);
            w.dev.reset();
            has_audio_.store(false);
            current_pts_us_.store(-1);
            duration_us_.store(0);
        }

        if (do_open) {
            release_decoder(w);
            w.dev.reset();
            w.pcm.clear();
            w.pcm_offset = 0;

            if (!create_device(w.dev)) {
                has_audio_.store(false);
                continue;
            }
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
            if (!open_audio_decoder(w, path)) {
                release_decoder(w);
                w.dev.reset();
                has_audio_.store(false);
                continue;
            }

            // Compute duration from av and stash.
            AVStream* st = w.fmt_ctx->streams[w.audio_stream];
            int64_t dur = 0;
            if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
                dur = av_rescale_q(st->duration, st->time_base,
                                   AVRational{1, 1'000'000});
            } else if (w.fmt_ctx->duration != AV_NOPTS_VALUE) {
                dur = av_rescale_q(w.fmt_ctx->duration,
                                   AVRational{1, AV_TIME_BASE},
                                   AVRational{1, 1'000'000});
            }
            duration_us_.store(core::TimeUs(dur));

            has_audio_.store(true);
            current_pts_us_.store(0);
            seek_target_.store(-1);
            w.stream_pts_us    = 0;
            w.clock_origin_us  = 0;
            w.clock_origin_pos = 0;
            w.clock_dirty      = true;
            w.device_running   = false;
            log::info("Audio engine ready: %u Hz, %u ch (libav)",
                      w.sample_rate, w.channels);
        }

        if (!has_audio_.load()) continue;

        // Apply pending seek.
        const core::TimeUs seek = seek_target_.exchange(-1);
        if (seek >= 0 && w.codec_ctx) {
            AVStream* st = w.fmt_ctx->streams[w.audio_stream];
            int64_t target_ts = av_rescale_q(int64_t(seek),
                AVRational{1, 1'000'000}, st->time_base) + w.ts_offset;
            int err = av_seek_frame(w.fmt_ctx, w.audio_stream, target_ts,
                                    AVSEEK_FLAG_BACKWARD);
            if (err < 0) {
                log::warn("Audio: av_seek_frame %s", averr(err).c_str());
            }
            avcodec_flush_buffers(w.codec_ctx);
            if (w.swr) swr_init(w.swr);  // drain resampler
            if (w.device_running) {
                w.dev.client->Stop();
                w.dev.client->Reset();
                w.device_running = false;
            }
            w.pcm.clear();
            w.pcm_offset      = 0;
            w.eof_reached     = false;
            w.stream_pts_us   = seek;
            w.clock_origin_us = seek;
            w.clock_dirty     = true;
            current_pts_us_.store(seek);
        }

        if (!playing_.load()) {
            if (w.device_running) {
                w.dev.client->Stop();
                w.dev.client->Reset();
                w.device_running = false;
            }
            w.clock_origin_us = w.stream_pts_us;
            w.clock_dirty     = true;
            continue;
        }
        if (!w.device_running) {
            if (SUCCEEDED(w.dev.client->Start())) {
                w.device_running = true;
                w.clock_origin_us = w.stream_pts_us;
                w.clock_dirty     = true;
            }
        }

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
            if (w.pcm_offset >= w.pcm.size()) {
                w.pcm.clear();
                w.pcm_offset = 0;
                if (!refill_pcm(w)) {
                    std::memset(out + produced, 0, want_bytes - produced);
                    produced = want_bytes;
                    break;
                }
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
            if (w.clock_dirty) {
                w.clock_origin_pos = pos;
                w.clock_dirty      = false;
            }
            const UINT64 delta_pos = (pos > w.clock_origin_pos)
                                   ? pos - w.clock_origin_pos
                                   : 0;
            const core::TimeUs played_us =
                core::TimeUs(double(delta_pos) * 1'000'000.0 / double(freq));
            current_pts_us_.store(w.clock_origin_us + played_us);
        } else {
            current_pts_us_.store(w.stream_pts_us);
        }
    }

    if (w.dev.client) w.dev.client->Stop();
    release_decoder(w);
    w.dev.reset();
    if (SUCCEEDED(init)) ::CoUninitialize();
}

}  // namespace volchay::media
