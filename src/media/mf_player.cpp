// libavformat / libavcodec / libswscale-based video player.
// (File name kept for source-compat with the previous Media Foundation
// implementation; the class is still called MfPlayer.)
#include "media/mf_player.h"

#include "util/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

namespace volchay::media {
namespace {

core::TimeUs av_to_us(int64_t pts, AVRational tb) {
    if (pts == AV_NOPTS_VALUE) return 0;
    return core::TimeUs(av_rescale_q(pts, tb, AVRational{1, 1'000'000}));
}

int64_t us_to_av(core::TimeUs us, AVRational tb) {
    return av_rescale_q(int64_t(us), AVRational{1, 1'000'000}, tb);
}

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

}  // namespace

MfPlayer::MfPlayer() {
    worker_ = std::thread(&MfPlayer::worker_main, this);
}

MfPlayer::~MfPlayer() {
    quit_.store(true);
    {
        std::lock_guard lk(mu_);
        close_request_pending_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

// ---------------------------------------------------------------------------
// UI thread API
// ---------------------------------------------------------------------------

bool MfPlayer::open(const std::wstring& path, ID3D11Device* device) {
    if (!device) return false;

    // Tear down any previous open synchronously.
    {
        std::lock_guard lk(mu_);
        close_request_pending_ = true;
    }
    cv_.notify_all();
    for (int i = 0; i < 200 && reader_open_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    device_ = ComPtr<ID3D11Device>(device);
    device_->GetImmediateContext(context_.put());
    source_path_ = path;
    current_pts_.store(-1);
    seek_target_.store(0);
    target_pts_.store(0);
    decode_kick_.store(true);

    open_done_.store(false);
    {
        std::lock_guard lk(mu_);
        pending_path_ = path;
        open_request_pending_ = true;
    }
    cv_.notify_all();

    // Block on metadata-probe phase. 5 s deadline so a hung decoder
    // can't permanently freeze the UI.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(5);
    while (!open_done_.load()
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return reader_open_.load();
}

void MfPlayer::close() {
    {
        std::lock_guard lk(mu_);
        close_request_pending_ = true;
    }
    cv_.notify_all();
    for (int i = 0; i < 1000 && reader_open_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    srv_.reset();
    texture_.reset();
    context_.reset();
    device_.reset();
    source_path_.clear();
    width_.store(0);
    height_.store(0);
    fps_.store(0.0);
    duration_.store(0);
    current_pts_.store(-1);
    hardware_decode_.store(false);
    {
        std::lock_guard lk(buf_mu_);
        shared_buf_.clear();
        shared_buf_w_ = 0;
        shared_buf_h_ = 0;
        shared_buf_pts_ = -1;
    }
    frame_ready_.store(false);
}

void MfPlayer::seek(core::TimeUs t) {
    if (!reader_open_.load()) return;
    if (t < 0) t = 0;
    const core::TimeUs dur = duration_.load();
    if (dur > 0 && t > dur) t = dur;
    seek_target_.store(t);
    target_pts_.store(t);
    decode_kick_.store(true);
    current_pts_.store(t);
    cv_.notify_all();
}

bool MfPlayer::pump(core::TimeUs playhead_us) {
    if (!reader_open_.load()) return false;
    target_pts_.store(playhead_us);

    bool uploaded = false;
    if (frame_ready_.exchange(false)) {
        std::lock_guard lk(buf_mu_);
        if (!shared_buf_.empty() && texture_ && context_) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HRESULT hr_map = context_->Map(texture_.get(), 0,
                                           D3D11_MAP_WRITE_DISCARD, 0, &mapped);
            if (SUCCEEDED(hr_map)) {
                const size_t row_bytes = size_t(shared_buf_w_) * 4;
                BYTE*       dst = (BYTE*)mapped.pData;
                const BYTE* src = shared_buf_.data();
                for (int y = 0; y < shared_buf_h_; ++y) {
                    std::memcpy(dst + size_t(y) * mapped.RowPitch,
                                src + size_t(y) * row_bytes,
                                row_bytes);
                }
                context_->Unmap(texture_.get(), 0);
                current_pts_.store(shared_buf_pts_);
                uploaded = true;
            } else {
                log::warn("Map(dynamic) failed 0x%08lx", long(hr_map));
            }
        }
    }

    const core::TimeUs cur = current_pts_.load();
    const double      f   = fps_.load();
    const core::TimeUs frame_time = (f > 0.0)
        ? core::TimeUs(1'000'000.0 / f)
        : core::TimeUs(16'000);
    if (cur < 0 || playhead_us >= cur + frame_time) {
        if (!decode_kick_.exchange(true)) {
            cv_.notify_all();
        }
    }
    return uploaded;
}

// ---------------------------------------------------------------------------
// Worker thread
// ---------------------------------------------------------------------------

void MfPlayer::worker_main() {
    while (!quit_.load()) {
        bool         do_open  = false;
        bool         do_close = false;
        std::wstring path;

        {
            std::unique_lock lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(20), [&] {
                return quit_.load()
                    || open_request_pending_
                    || close_request_pending_
                    || decode_kick_.load();
            });
            if (open_request_pending_) {
                do_open = true;
                open_request_pending_ = false;
                path = pending_path_;
            }
            if (close_request_pending_) {
                do_close = true;
                close_request_pending_ = false;
            }
        }

        if (do_close) {
            worker_release_decoder();
        }
        if (do_open) {
            worker_release_decoder();
            if (!worker_open(path)) {
                log::err("FFmpeg worker: open failed");
                worker_release_decoder();
            }
            open_done_.store(true);
        }

        if (!reader_open_.load()) continue;

        // Apply pending seek before decode.
        const core::TimeUs seek = seek_target_.exchange(-1);
        if (seek >= 0 && fmt_ctx_) {
            AVStream* st = fmt_ctx_->streams[video_stream_];
            int64_t target_ts = us_to_av(seek, st->time_base) + ts_offset_;
            int err = av_seek_frame(fmt_ctx_, video_stream_, target_ts,
                                    AVSEEK_FLAG_BACKWARD);
            if (err < 0) {
                log::warn("av_seek_frame: %s", averr(err).c_str());
            }
            avcodec_flush_buffers(codec_ctx_);
            worker_pts_ = -1;
        }

        if (decode_kick_.exchange(false)) {
            const core::TimeUs target = target_pts_.load();
            worker_decode_one(target);
        }
    }

    worker_release_decoder();
}

bool MfPlayer::worker_open(const std::wstring& path) {
    if (!device_) return false;

    const std::string utf8_path = narrow_path(path);

    AVFormatContext* fmt = nullptr;
    int err = avformat_open_input(&fmt, utf8_path.c_str(), nullptr, nullptr);
    if (err < 0) {
        log::err("avformat_open_input: %s", averr(err).c_str());
        return false;
    }
    fmt_ctx_ = fmt;

    err = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (err < 0) {
        log::err("avformat_find_stream_info: %s", averr(err).c_str());
        return false;
    }

    int vstream = av_find_best_stream(fmt_ctx_, AVMEDIA_TYPE_VIDEO,
                                      -1, -1, nullptr, 0);
    if (vstream < 0) {
        log::err("No video stream found");
        return false;
    }
    video_stream_ = vstream;
    AVStream* st = fmt_ctx_->streams[video_stream_];

    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        log::err("No decoder for codec id %d", int(st->codecpar->codec_id));
        return false;
    }
    codec_ctx_ = avcodec_alloc_context3(dec);
    if (!codec_ctx_) return false;
    err = avcodec_parameters_to_context(codec_ctx_, st->codecpar);
    if (err < 0) {
        log::err("avcodec_parameters_to_context: %s", averr(err).c_str());
        return false;
    }
    codec_ctx_->thread_count = 0;  // auto: ~ncpu
    codec_ctx_->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;

    err = avcodec_open2(codec_ctx_, dec, nullptr);
    if (err < 0) {
        log::err("avcodec_open2: %s", averr(err).c_str());
        return false;
    }

    int w = codec_ctx_->width;
    int h = codec_ctx_->height;
    if (w <= 0 || h <= 0) {
        log::err("Decoder reports zero dimensions");
        return false;
    }
    width_.store(w);
    height_.store(h);

    AVRational fr = av_guess_frame_rate(fmt_ctx_, st, nullptr);
    double fps = (fr.den != 0) ? (double(fr.num) / double(fr.den)) : 0.0;
    if (fps <= 0.0 && st->avg_frame_rate.den != 0) {
        fps = double(st->avg_frame_rate.num) / double(st->avg_frame_rate.den);
    }
    if (fps <= 0.0) fps = 30.0;
    fps_.store(fps);

    int64_t dur = 0;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0) {
        dur = av_rescale_q(st->duration, st->time_base,
                           AVRational{1, 1'000'000});
    } else if (fmt_ctx_->duration != AV_NOPTS_VALUE) {
        dur = av_rescale_q(fmt_ctx_->duration, AVRational{1, AV_TIME_BASE},
                           AVRational{1, 1'000'000});
    }
    duration_.store(core::TimeUs(dur));

    // Some formats start at non-zero PTS (e.g. transport streams);
    // remember the offset so seek/clock report time-from-start.
    ts_offset_ = (st->start_time != AV_NOPTS_VALUE) ? st->start_time : 0;

    // Allocate reusable packet/frame and the BGRA scaler.
    packet_ = av_packet_alloc();
    frame_  = av_frame_alloc();
    if (!packet_ || !frame_) return false;

    sws_ctx_ = sws_getContext(
        w, h, codec_ctx_->pix_fmt,
        w, h, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        log::err("sws_getContext failed for src fmt %d (%s)",
                 int(codec_ctx_->pix_fmt),
                 av_get_pix_fmt_name(codec_ctx_->pix_fmt)
                    ? av_get_pix_fmt_name(codec_ctx_->pix_fmt) : "?");
        return false;
    }

    // Create the dynamic texture.
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = UINT(w);
    td.Height           = UINT(h);
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DYNAMIC;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags   = D3D11_CPU_ACCESS_WRITE;

    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, tex.put());
    if (FAILED(hr)) {
        log::err("CreateTexture2D(dynamic) failed 0x%08lx", long(hr));
        return false;
    }
    hr = device_->CreateShaderResourceView(tex.get(), nullptr, srv.put());
    if (FAILED(hr)) {
        log::err("CreateShaderResourceView 0x%08lx", long(hr));
        return false;
    }
    texture_ = tex;
    srv_     = srv;

    {
        std::lock_guard lk(buf_mu_);
        shared_buf_.assign(size_t(w) * size_t(h) * 4, 0);
        shared_buf_w_   = w;
        shared_buf_h_   = h;
        shared_buf_pts_ = -1;
    }
    frame_ready_.store(false);
    worker_pts_ = -1;

    log::info("FF stream: %dx%d @ %.3f fps, codec %s, pix_fmt %s, duration %.3fs",
              w, h, fps, dec->name,
              av_get_pix_fmt_name(codec_ctx_->pix_fmt)
                ? av_get_pix_fmt_name(codec_ctx_->pix_fmt) : "?",
              double(dur) / 1'000'000.0);
    log::info("Video texture ready: %dx%d BGRA8 (dynamic)", w, h);
    reader_open_.store(true);
    return true;
}

void MfPlayer::worker_release_decoder() {
    reader_open_.store(false);
    if (sws_ctx_)   { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (frame_)     { av_frame_free(&frame_);    frame_   = nullptr; }
    if (packet_)    { av_packet_free(&packet_);  packet_  = nullptr; }
    if (codec_ctx_) { avcodec_free_context(&codec_ctx_);              }
    if (fmt_ctx_)   { avformat_close_input(&fmt_ctx_);                }
    video_stream_ = -1;
    width_.store(0);
    height_.store(0);
    fps_.store(0.0);
    duration_.store(0);
    hardware_decode_.store(false);
    worker_pts_ = -1;
    ts_offset_  = 0;
}

bool MfPlayer::worker_decode_one(core::TimeUs target_us) {
    if (!fmt_ctx_ || !codec_ctx_ || !packet_ || !frame_ || !sws_ctx_)
        return false;

    AVStream* st = fmt_ctx_->streams[video_stream_];

    int sample_count = 0;
    while (true) {
        if (++sample_count > 256) {
            log::warn("worker_decode_one: 256 packets without a frame — bailing");
            return false;
        }

        // First try draining decoder output.
        int err = avcodec_receive_frame(codec_ctx_, frame_);
        if (err == 0) {
            // Got a frame.
            int64_t pts_ts = (frame_->best_effort_timestamp != AV_NOPTS_VALUE)
                ? frame_->best_effort_timestamp
                : frame_->pts;
            if (pts_ts == AV_NOPTS_VALUE) pts_ts = ts_offset_;
            const core::TimeUs pts =
                av_to_us(pts_ts - ts_offset_, st->time_base);

            // Drop frames that are well behind the playhead so we can
            // catch up after a seek without freezing the UI on each one.
            const double f = fps_.load();
            if (f > 0.0 && pts + core::TimeUs(1'000'000.0 / f) < target_us
                && worker_pts_ >= 0) {
                av_frame_unref(frame_);
                continue;
            }

            // Convert to BGRA into the back buffer.
            const int w = shared_buf_w_;
            const int h = shared_buf_h_;
            uint8_t* dst[4]    = { shared_buf_.data(), nullptr, nullptr, nullptr };
            int      dst_lin[4]= { w * 4, 0, 0, 0 };
            {
                std::lock_guard lk(buf_mu_);
                int got = sws_scale(sws_ctx_,
                                    frame_->data, frame_->linesize,
                                    0, frame_->height,
                                    dst, dst_lin);
                if (got <= 0) {
                    log::warn("sws_scale produced %d rows", got);
                    av_frame_unref(frame_);
                    return false;
                }
                shared_buf_pts_ = pts;
            }
            const bool first = (worker_pts_ < 0);
            worker_pts_ = pts;
            frame_ready_.store(true);
            if (first) {
                log::info("First frame decoded (pts=%.3fs, %dx%d, %s)",
                          double(pts) / 1'000'000.0,
                          w, h,
                          codec_ctx_->codec ? codec_ctx_->codec->name : "?");
            }
            av_frame_unref(frame_);
            return true;
        }
        if (err != AVERROR(EAGAIN) && err != AVERROR_EOF) {
            log::warn("avcodec_receive_frame: %s", averr(err).c_str());
            return false;
        }

        if (err == AVERROR_EOF) {
            log::info("Decoder reached end of stream");
            return false;
        }

        // Need more data — read another packet.
        err = av_read_frame(fmt_ctx_, packet_);
        if (err == AVERROR_EOF) {
            // Flush.
            avcodec_send_packet(codec_ctx_, nullptr);
            continue;
        }
        if (err < 0) {
            log::warn("av_read_frame: %s", averr(err).c_str());
            return false;
        }
        if (packet_->stream_index != video_stream_) {
            av_packet_unref(packet_);
            continue;
        }
        err = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        if (err < 0 && err != AVERROR(EAGAIN)) {
            log::warn("avcodec_send_packet: %s", averr(err).c_str());
            return false;
        }
        // Loop back to receive_frame.
    }
}

}  // namespace volchay::media
