// FFmpeg/libav-based exporter.
//
// Reads the open file via libavformat (which gave us a working AV1 +
// HEVC decode path for preview), re-encodes video to H.264 or HEVC and
// audio to AAC, muxes into .mp4. The previous IMFSinkWriter version
// kept failing on `WriteSample(video) hr=0x80070057` for AV1 sources
// regardless of the negotiated NV12 input type.
//
// Encoder selection: we try the GPU encoders first (h264_nvenc /
// h264_qsv / h264_amf / h264_mf, and the hevc equivalents), then fall
// back to the openh264 software encoder. Whatever the user passed for
// req.hardware is honoured as a preference, but if the GPU-resident
// encoders aren't present in the BtbN LGPL build (or aren't available
// on this hardware) the exporter still finishes via software.
#include "media/exporter.h"

#include "core/project.h"
#include "util/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace volchay::media {

// Display-only labels for the dialog's "Quick presets" list. The
// underlying ExportRequest fields are what the encoder actually sees;
// these just pre-fill them.
const ExportPreset kPresets[] = {
    {"H.264 1080p / 12 Mbps",         1920, 1080, 12'000'000, 0, 1, false},
    {"H.264 1080p / 8 Mbps  (web)",   1920, 1080,  8'000'000, 0, 1, false},
    {"H.264 4K   / 35 Mbps",          3840, 2160, 35'000'000, 0, 1, false},
    {"HEVC 4K HDR / 50 Mbps",         3840, 2160, 50'000'000, 0, 1, true},
    {"H.264 720p / 5 Mbps  (mobile)", 1280,  720,  5'000'000, 30, 1, false},
};
const int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

namespace {

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

// Encoder lookup helper. Tries the named encoders in order and returns
// the first one that exists in this libavcodec build.
const AVCodec* find_encoder(const char* const* names, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (!names[i]) continue;
        const AVCodec* c = avcodec_find_encoder_by_name(names[i]);
        if (c) return c;
    }
    return nullptr;
}

}  // namespace

Exporter::Exporter() {
    worker_ = std::thread(&Exporter::worker_main, this);
}

Exporter::~Exporter() {
    quit_.store(true);
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
}

bool Exporter::start(const ExportRequest& req) {
    if (busy_.load()) return false;
    {
        std::lock_guard lk(mu_);
        pending_     = req;
        has_pending_ = true;
        status_      = "Starting...";
    }
    busy_.store(true);
    cancel_.store(false);
    finished_.store(false);
    success_.store(false);
    progress_.store(0.0f);
    return true;
}

void Exporter::cancel() {
    cancel_.store(true);
}

std::string Exporter::status_text() const {
    std::lock_guard lk(const_cast<std::mutex&>(mu_));
    return status_;
}

ExportRequest Exporter::last_request() const {
    std::lock_guard lk(const_cast<std::mutex&>(mu_));
    return last_;
}

void Exporter::mark_replaced() {
    std::lock_guard lk(mu_);
    last_.replace_source = false;
    last_.replace_final_path.clear();
}

void Exporter::worker_main() {
    while (!quit_.load()) {
        ExportRequest req;
        bool         have = false;
        {
            std::lock_guard lk(mu_);
            if (has_pending_) {
                req = pending_;
                has_pending_ = false;
                have = true;
            }
        }
        if (have) {
            run_one_export(req);
            {
                std::lock_guard lk(mu_);
                last_ = req;
            }
            busy_.store(false);
            finished_.store(true);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

// ---------------------------------------------------------------------------
// Audio-only extract. Pulls the first audio stream from `req.source_path`,
// re-encodes it to AAC, muxes into .m4a / .aac at `req.output_path`. No
// video stream is touched. Used by File > Extract audio.
// ---------------------------------------------------------------------------
static void run_audio_only_export(const ExportRequest& req,
                                  std::atomic<float>& progress,
                                  std::atomic<bool>&  cancel,
                                  std::atomic<bool>&  success,
                                  std::function<void(std::string)> set_status) {
    set_status("Opening source...");
    AVFormatContext* in_fmt = nullptr;
    int err = avformat_open_input(&in_fmt,
        narrow_path(req.source_path).c_str(), nullptr, nullptr);
    if (err < 0) { set_status("Cannot open source: " + averr(err)); return; }
    if (avformat_find_stream_info(in_fmt, nullptr) < 0) {
        set_status("Cannot probe streams"); avformat_close_input(&in_fmt); return;
    }

    int a_idx = av_find_best_stream(in_fmt, AVMEDIA_TYPE_AUDIO,
                                    -1, -1, nullptr, 0);
    if (a_idx < 0) {
        set_status("No audio stream in source");
        avformat_close_input(&in_fmt);
        return;
    }
    AVStream* a_in = in_fmt->streams[a_idx];
    const AVCodec* a_dec = avcodec_find_decoder(a_in->codecpar->codec_id);
    if (!a_dec) {
        set_status("No decoder for audio codec");
        avformat_close_input(&in_fmt);
        return;
    }
    AVCodecContext* a_dec_ctx = avcodec_alloc_context3(a_dec);
    avcodec_parameters_to_context(a_dec_ctx, a_in->codecpar);
    if (avcodec_open2(a_dec_ctx, a_dec, nullptr) < 0) {
        set_status("Open audio decoder failed");
        avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }

    set_status("Opening output...");
    const std::string out_path = narrow_path(req.output_path);
    // mp4/m4a both work for AAC. Pick by extension if .aac, otherwise m4a.
    const char* fmt_name = "mp4";
    if (out_path.size() >= 4) {
        const char* ext = out_path.c_str() + out_path.size() - 4;
        if (_stricmp(ext, ".aac") == 0) fmt_name = "adts";
    }
    AVFormatContext* out_fmt = nullptr;
    err = avformat_alloc_output_context2(&out_fmt, nullptr, fmt_name,
                                         out_path.c_str());
    if (err < 0 || !out_fmt) {
        set_status("Create output failed: " + averr(err));
        avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }

    const AVCodec* a_enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!a_enc) {
        set_status("No AAC encoder available");
        avformat_free_context(out_fmt);
        avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }
    AVCodecContext* a_enc_ctx = avcodec_alloc_context3(a_enc);
    a_enc_ctx->sample_rate = (a_dec_ctx->sample_rate > 0)
                             ? a_dec_ctx->sample_rate : 48000;
    a_enc_ctx->bit_rate    = req.audio_bitrate ? req.audio_bitrate : 192000;
    a_enc_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&a_enc_ctx->ch_layout, 2);
    a_enc_ctx->time_base   = AVRational{1, a_enc_ctx->sample_rate};
    if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        a_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(a_enc_ctx, a_enc, nullptr) < 0) {
        set_status("Open AAC encoder failed");
        avcodec_free_context(&a_enc_ctx);
        avformat_free_context(out_fmt);
        avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }
    AVStream* a_out = avformat_new_stream(out_fmt, nullptr);
    avcodec_parameters_from_context(a_out->codecpar, a_enc_ctx);
    a_out->time_base = a_enc_ctx->time_base;

    SwrContext* swr = nullptr;
    AVChannelLayout in_layout;
    if (a_dec_ctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
        av_channel_layout_copy(&in_layout, &a_dec_ctx->ch_layout);
    } else {
        av_channel_layout_default(&in_layout,
                                  a_dec_ctx->ch_layout.nb_channels);
    }
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, 2);
    swr_alloc_set_opts2(&swr,
        &out_layout, a_enc_ctx->sample_fmt, a_enc_ctx->sample_rate,
        &in_layout,  a_dec_ctx->sample_fmt, a_dec_ctx->sample_rate,
        0, nullptr);
    if (!swr || swr_init(swr) < 0) {
        set_status("Resampler init failed");
        if (swr) swr_free(&swr);
        avcodec_free_context(&a_enc_ctx);
        avformat_free_context(out_fmt);
        avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }
    av_channel_layout_uninit(&in_layout);
    av_channel_layout_uninit(&out_layout);

    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        err = avio_open(&out_fmt->pb, out_path.c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            set_status("avio_open failed: " + averr(err));
            swr_free(&swr);
            avcodec_free_context(&a_enc_ctx);
            avformat_free_context(out_fmt);
            avcodec_free_context(&a_dec_ctx);
            avformat_close_input(&in_fmt);
            return;
        }
    }
    if (avformat_write_header(out_fmt, nullptr) < 0) {
        set_status("write_header failed");
        if (out_fmt->pb) avio_closep(&out_fmt->pb);
        swr_free(&swr);
        avcodec_free_context(&a_enc_ctx);
        avformat_free_context(out_fmt);
        avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }

    int64_t total_us = 0;
    if (in_fmt->duration != AV_NOPTS_VALUE) {
        total_us = av_rescale_q(in_fmt->duration,
                                AVRational{1, AV_TIME_BASE},
                                AVRational{1, 1'000'000});
    }
    if (total_us <= 0) total_us = 1;

    AVPacket* pkt    = av_packet_alloc();
    AVPacket* enc_pkt= av_packet_alloc();
    AVFrame*  in_f   = av_frame_alloc();
    AVFrame*  out_f  = av_frame_alloc();
    out_f->format         = a_enc_ctx->sample_fmt;
    out_f->sample_rate    = a_enc_ctx->sample_rate;
    av_channel_layout_copy(&out_f->ch_layout, &a_enc_ctx->ch_layout);
    out_f->nb_samples     = a_enc_ctx->frame_size > 0 ? a_enc_ctx->frame_size : 1024;
    av_frame_get_buffer(out_f, 0);

    int64_t a_pts_count = 0;
    bool failed = false;

    auto encode_frame = [&](AVFrame* f) -> int {
        int rc = avcodec_send_frame(a_enc_ctx, f);
        if (rc < 0 && rc != AVERROR(EAGAIN)) return rc;
        while (true) {
            rc = avcodec_receive_packet(a_enc_ctx, enc_pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return rc;
            enc_pkt->stream_index = a_out->index;
            av_packet_rescale_ts(enc_pkt, a_enc_ctx->time_base,
                                 a_out->time_base);
            int wr = av_interleaved_write_frame(out_fmt, enc_pkt);
            av_packet_unref(enc_pkt);
            if (wr < 0) return wr;
        }
        return 0;
    };

    while (!cancel.load()) {
        err = av_read_frame(in_fmt, pkt);
        if (err < 0) break;
        if (pkt->stream_index != a_idx) {
            av_packet_unref(pkt);
            continue;
        }
        int rc = avcodec_send_packet(a_dec_ctx, pkt);
        av_packet_unref(pkt);
        if (rc < 0 && rc != AVERROR(EAGAIN)) continue;
        while (true) {
            rc = avcodec_receive_frame(a_dec_ctx, in_f);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) { failed = true; break; }
            int64_t pts_us = av_rescale_q(in_f->best_effort_timestamp,
                                          a_in->time_base,
                                          AVRational{1, 1'000'000});
            // Resample into out_f-sized chunks.
            const uint8_t** in_data = (const uint8_t**)in_f->data;
            int in_nb = in_f->nb_samples;
            while (in_nb > 0 || swr_get_delay(swr, a_enc_ctx->sample_rate)) {
                int got = swr_convert(swr, out_f->data, out_f->nb_samples,
                                      in_data, in_nb);
                in_data = nullptr;
                in_nb   = 0;
                if (got <= 0) break;
                out_f->pts        = a_pts_count;
                out_f->nb_samples = got;
                a_pts_count += got;
                int wr = encode_frame(out_f);
                if (wr < 0) { failed = true; break; }
            }
            av_frame_unref(in_f);
            if (total_us > 0) {
                progress.store(std::clamp(float(double(pts_us) / total_us),
                                          0.0f, 0.99f));
            }
        }
        if (failed) break;
    }
    encode_frame(nullptr);  // flush
    av_write_trailer(out_fmt);

    av_packet_free(&pkt);
    av_packet_free(&enc_pkt);
    av_frame_free(&in_f);
    av_frame_free(&out_f);
    swr_free(&swr);
    if (out_fmt->pb) avio_closep(&out_fmt->pb);
    avcodec_free_context(&a_enc_ctx);
    avformat_free_context(out_fmt);
    avcodec_free_context(&a_dec_ctx);
    avformat_close_input(&in_fmt);

    if (!failed && !cancel.load()) {
        progress.store(1.0f);
        success.store(true);
        set_status("Audio extracted.");
    } else {
        set_status(cancel.load() ? "Cancelled." : "Audio extract failed.");
    }
}

// ---------------------------------------------------------------------------
// Multi-track / multi-source project export. Walks the timeline at output
// FPS, decodes each layer's source frame on demand, and composites every
// active clip with its transform into the encoder canvas. Falls back to
// the single-source `run_one_export` body when the project is purely
// single-track + single-source (so the simple/fast path still wins for
// trivial timelines).
// ---------------------------------------------------------------------------
namespace {

struct VideoSource {
    AVFormatContext* fmt = nullptr;
    AVCodecContext*  dec = nullptr;
    int              v_idx = -1;
    AVStream*        v_stream = nullptr;
    AVFrame*         current = nullptr;     // last successfully decoded frame
    int64_t          last_pts_us = AV_NOPTS_VALUE;
    bool             eof = false;

    ~VideoSource() {
        if (current) av_frame_free(&current);
        if (dec) avcodec_free_context(&dec);
        if (fmt) avformat_close_input(&fmt);
    }

    bool open(const std::string& path_narrow) {
        int err = avformat_open_input(&fmt, path_narrow.c_str(),
                                      nullptr, nullptr);
        if (err < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        v_idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1,
                                    nullptr, 0);
        if (v_idx < 0) return false;
        v_stream = fmt->streams[v_idx];
        const AVCodec* d = avcodec_find_decoder(v_stream->codecpar->codec_id);
        if (!d) return false;
        dec = avcodec_alloc_context3(d);
        avcodec_parameters_to_context(dec, v_stream->codecpar);
        dec->thread_count = 0;
        if (avcodec_open2(dec, d, nullptr) < 0) return false;
        return true;
    }

    // Return the most recent decoded frame whose pts_us <= target, or the
    // first frame with pts_us >= target if we have to read forward.
    // Returns nullptr only on terminal failure / EOF before any frame.
    AVFrame* frame_at(int64_t target_us) {
        // Seek backwards if we're materially past the requested time.
        if (last_pts_us != AV_NOPTS_VALUE && last_pts_us > target_us + 200'000) {
            int64_t seek_ts = av_rescale_q(
                std::max<int64_t>(0, target_us - 200'000),
                AVRational{1, 1'000'000}, v_stream->time_base);
            av_seek_frame(fmt, v_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(dec);
            last_pts_us = AV_NOPTS_VALUE;
            eof = false;
        }
        // Read forward until we have a frame whose pts >= target_us, or eof.
        AVPacket* pkt = av_packet_alloc();
        AVFrame*  tmp = av_frame_alloc();
        while (!eof && (last_pts_us == AV_NOPTS_VALUE
                        || last_pts_us < target_us)) {
            int rc = av_read_frame(fmt, pkt);
            if (rc < 0) {
                avcodec_send_packet(dec, nullptr);
                rc = avcodec_receive_frame(dec, tmp);
                if (rc < 0) { eof = true; break; }
            } else {
                if (pkt->stream_index != v_idx) {
                    av_packet_unref(pkt);
                    continue;
                }
                avcodec_send_packet(dec, pkt);
                av_packet_unref(pkt);
                rc = avcodec_receive_frame(dec, tmp);
                if (rc == AVERROR(EAGAIN)) continue;
                if (rc < 0) { eof = true; break; }
            }
            int64_t pts_ts = (tmp->best_effort_timestamp != AV_NOPTS_VALUE)
                ? tmp->best_effort_timestamp : tmp->pts;
            int64_t pts_us = av_rescale_q(pts_ts, v_stream->time_base,
                                          AVRational{1, 1'000'000});
            if (current) av_frame_free(&current);
            current = tmp;
            tmp = av_frame_alloc();
            last_pts_us = pts_us;
        }
        av_packet_free(&pkt);
        av_frame_free(&tmp);
        return current;
    }
};

// Helper to compute plane byte offsets for arbitrary YUV planar / NV12
// frames. Mirrors the lambda in the single-source exporter so we can
// share per-frame compositing logic.
inline void plane_off(const AVFrame* f, int off_x, int off_y,
                      uint8_t* out_data[4], int out_ls[4]) {
    const AVPixelFormat fmt = (AVPixelFormat)f->format;
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    int bps[4]  = {1, 1, 1, 1};
    int hsub[4] = {0, 0, 0, 0};
    int vsub[4] = {0, 0, 0, 0};
    bool has[4] = {false, false, false, false};
    if (d) {
        for (int c = 0; c < d->nb_components; ++c) {
            int p = d->comp[c].plane;
            if (p < 0 || p >= 4) continue;
            bps[p] = std::max(bps[p], int(d->comp[c].step));
            if (c == 1 || c == 2) {
                hsub[p] = d->log2_chroma_w;
                vsub[p] = d->log2_chroma_h;
            }
            has[p] = true;
        }
    }
    for (int p = 0; p < 4; ++p) {
        out_ls[p] = f->linesize[p];
        if (!f->data[p] || !has[p]) {
            out_data[p] = f->data[p];
            continue;
        }
        out_data[p] = f->data[p]
            + (off_y >> vsub[p]) * f->linesize[p]
            + (off_x >> hsub[p]) * bps[p];
    }
}

inline void canvas_black(AVFrame* f) {
    const AVPixelFormat fmt = (AVPixelFormat)f->format;
    if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUV422P
     || fmt == AV_PIX_FMT_YUV444P) {
        std::memset(f->data[0], 0,   f->linesize[0] * f->height);
        std::memset(f->data[1], 128, f->linesize[1] * f->height);
        std::memset(f->data[2], 128, f->linesize[2] * f->height);
    } else if (fmt == AV_PIX_FMT_NV12) {
        std::memset(f->data[0], 0,   f->linesize[0] * f->height);
        std::memset(f->data[1], 128, f->linesize[1] * f->height);
    }
}

}  // namespace

static void run_project_export(ExportRequest req,
                               std::atomic<float>& progress,
                               std::atomic<bool>&  cancel,
                               std::atomic<bool>&  success,
                               std::function<void(std::string)> set_status) {
    using core::Project;
    using core::Clip;
    using core::Media;
    using core::TimeUs;
    const Project& proj = *req.project;
    if (proj.clips().empty()) {
        set_status("Project has no clips to export.");
        return;
    }

    // Output time base: one tick per output frame.
    int fps_num = (req.fps_num > 0) ? req.fps_num : 30;
    int fps_den = (req.fps_den > 0) ? req.fps_den : 1;
    const TimeUs frame_us = TimeUs(int64_t(1'000'000) * fps_den / fps_num);

    // Output frame range.
    if (req.trim_end_us < 0) req.trim_end_us = proj.duration();
    const TimeUs span_us = std::max<TimeUs>(1,
        req.trim_end_us - req.trim_start_us);
    const int64_t total_frames = std::max<int64_t>(1, span_us / frame_us);

    // Output dimensions.
    int out_w = (req.width  > 0) ? req.width  : 1920;
    int out_h = (req.height > 0) ? req.height : 1080;
    out_w &= ~1;
    out_h &= ~1;

    set_status("Opening output...");
    const std::string out_path = narrow_path(req.output_path);
    AVFormatContext* out_fmt = nullptr;
    int err = avformat_alloc_output_context2(&out_fmt, nullptr, "mp4",
                                             out_path.c_str());
    if (err < 0 || !out_fmt) {
        set_status("Cannot create output: " + averr(err));
        return;
    }

    // Video encoder.
    const char* h264_hw[] = { "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf" };
    const char* h264_sw[] = { "libopenh264", "h264" };
    const char* hevc_hw[] = { "hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_mf" };
    const char* hevc_sw[] = { "hevc", "libx265" };
    const AVCodec* v_enc = nullptr;
    if (req.codec == ExportCodec::HEVC) {
        if (req.hardware) v_enc = find_encoder(hevc_hw, sizeof(hevc_hw)/sizeof(*hevc_hw));
        if (!v_enc)       v_enc = find_encoder(hevc_sw, sizeof(hevc_sw)/sizeof(*hevc_sw));
    } else {
        if (req.hardware) v_enc = find_encoder(h264_hw, sizeof(h264_hw)/sizeof(*h264_hw));
        if (!v_enc)       v_enc = find_encoder(h264_sw, sizeof(h264_sw)/sizeof(*h264_sw));
    }
    if (!v_enc) {
        set_status("No video encoder available");
        avformat_free_context(out_fmt);
        return;
    }

    AVCodecContext* v_enc_ctx = avcodec_alloc_context3(v_enc);
    v_enc_ctx->width  = out_w;
    v_enc_ctx->height = out_h;
    v_enc_ctx->bit_rate = req.video_bitrate;
    v_enc_ctx->pix_fmt  = AV_PIX_FMT_YUV420P;
    if (std::string(v_enc->name).find("nvenc") != std::string::npos
     || std::string(v_enc->name).find("amf")   != std::string::npos
     || std::string(v_enc->name).find("qsv")   != std::string::npos) {
        v_enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    }
    v_enc_ctx->time_base    = AVRational{fps_den, fps_num};
    v_enc_ctx->framerate    = AVRational{fps_num, fps_den};
    v_enc_ctx->gop_size     = std::max(1, fps_num * 2 / fps_den);
    v_enc_ctx->max_b_frames = 2;
    if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        v_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    AVDictionary* enc_opts = nullptr;
    if (std::string(v_enc->name).find("nvenc") != std::string::npos) {
        av_dict_set(&enc_opts, "preset", "p4", 0);
        av_dict_set(&enc_opts, "tune",   "hq", 0);
    }
    if (avcodec_open2(v_enc_ctx, v_enc, &enc_opts) < 0) {
        av_dict_free(&enc_opts);
        set_status("Open video encoder failed");
        avcodec_free_context(&v_enc_ctx);
        avformat_free_context(out_fmt);
        return;
    }
    av_dict_free(&enc_opts);
    AVStream* v_out = avformat_new_stream(out_fmt, nullptr);
    avcodec_parameters_from_context(v_out->codecpar, v_enc_ctx);
    v_out->time_base = v_enc_ctx->time_base;
    log::info("ProjectExporter: encoder = %s, %dx%d @ %d/%d fps, pix=%s",
              v_enc->name, out_w, out_h, fps_num, fps_den,
              av_get_pix_fmt_name(v_enc_ctx->pix_fmt));

    // Pick audio source. Priority:
    //   1) An A-track clip (track < 0) — that's where "Extract audio"
    //      moved the user's audio after detaching it from a video.
    //      If there are several, prefer the highest A-track index
    //      (closest to A1 == -1) since that's the visual "top" audio
    //      lane and matches the user's intuition of A1 being primary.
    //   2) Otherwise, the lowest non-muted V-track clip — which is the
    //      typical V0-master case with V1+ being PIP overlays.
    // Fully-muted clips never act as audio sources.
    const Clip* audio_clip = nullptr;
    for (const auto& c : proj.clips()) {
        if (c.track >= 0) continue;
        if (c.muted) continue;
        if (c.t_in >= req.trim_end_us) continue;
        if (c.t_out() <= req.trim_start_us) continue;
        // Higher (less negative) wins — A1 (-1) > A2 (-2) > ...
        if (!audio_clip || c.track > audio_clip->track) audio_clip = &c;
    }
    if (!audio_clip) {
        for (const auto& c : proj.clips()) {
            if (c.track < 0) continue;
            if (c.muted) continue;
            if (c.t_in >= req.trim_end_us) continue;
            if (c.t_out() <= req.trim_start_us) continue;
            if (!audio_clip || c.track < audio_clip->track) audio_clip = &c;
        }
    }
    AVFormatContext* a_in_fmt = nullptr;
    AVCodecContext*  a_dec_ctx = nullptr;
    AVStream*        a_in_stream = nullptr;
    int              a_in_idx = -1;
    AVCodecContext*  a_enc_ctx = nullptr;
    AVStream*        a_out_stream = nullptr;
    SwrContext*      swr = nullptr;
    if (audio_clip) {
        const Media* m = proj.find_media(audio_clip->media_id);
        if (m) {
            std::string ap = narrow_path(m->path);
            if (avformat_open_input(&a_in_fmt, ap.c_str(),
                                    nullptr, nullptr) >= 0
             && avformat_find_stream_info(a_in_fmt, nullptr) >= 0) {
                a_in_idx = av_find_best_stream(a_in_fmt, AVMEDIA_TYPE_AUDIO,
                                               -1, -1, nullptr, 0);
                if (a_in_idx >= 0) {
                    a_in_stream = a_in_fmt->streams[a_in_idx];
                    const AVCodec* dec = avcodec_find_decoder(
                        a_in_stream->codecpar->codec_id);
                    if (dec) {
                        a_dec_ctx = avcodec_alloc_context3(dec);
                        avcodec_parameters_to_context(a_dec_ctx,
                            a_in_stream->codecpar);
                        if (avcodec_open2(a_dec_ctx, dec, nullptr) < 0) {
                            avcodec_free_context(&a_dec_ctx);
                        }
                    }
                }
            }
            if (!a_dec_ctx && a_in_fmt) avformat_close_input(&a_in_fmt);
        }
    }
    if (a_dec_ctx) {
        const AVCodec* a_enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (a_enc) {
            a_enc_ctx = avcodec_alloc_context3(a_enc);
            a_enc_ctx->sample_rate = 48000;
            a_enc_ctx->bit_rate    = req.audio_bitrate;
            a_enc_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&a_enc_ctx->ch_layout, 2);
            a_enc_ctx->time_base   = AVRational{1, a_enc_ctx->sample_rate};
            if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
                a_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            if (avcodec_open2(a_enc_ctx, a_enc, nullptr) < 0) {
                avcodec_free_context(&a_enc_ctx);
            } else {
                a_out_stream = avformat_new_stream(out_fmt, nullptr);
                avcodec_parameters_from_context(a_out_stream->codecpar,
                                                a_enc_ctx);
                a_out_stream->time_base = a_enc_ctx->time_base;

                AVChannelLayout in_layout, out_layout;
                if (a_dec_ctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
                    av_channel_layout_copy(&in_layout, &a_dec_ctx->ch_layout);
                } else {
                    av_channel_layout_default(&in_layout,
                        a_dec_ctx->ch_layout.nb_channels);
                }
                av_channel_layout_default(&out_layout, 2);
                swr_alloc_set_opts2(&swr,
                    &out_layout, a_enc_ctx->sample_fmt, a_enc_ctx->sample_rate,
                    &in_layout,  a_dec_ctx->sample_fmt, a_dec_ctx->sample_rate,
                    0, nullptr);
                if (!swr || swr_init(swr) < 0) {
                    if (swr) swr_free(&swr);
                    avcodec_free_context(&a_enc_ctx);
                    a_out_stream = nullptr;
                }
                av_channel_layout_uninit(&in_layout);
                av_channel_layout_uninit(&out_layout);
            }
        }
    }

    // Open output file.
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt->pb, out_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            set_status("avio_open failed");
            if (swr) swr_free(&swr);
            if (a_enc_ctx) avcodec_free_context(&a_enc_ctx);
            if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
            if (a_in_fmt) avformat_close_input(&a_in_fmt);
            avcodec_free_context(&v_enc_ctx);
            avformat_free_context(out_fmt);
            return;
        }
    }
    if (avformat_write_header(out_fmt, nullptr) < 0) {
        set_status("write_header failed");
        if (out_fmt->pb) avio_closep(&out_fmt->pb);
        if (swr) swr_free(&swr);
        if (a_enc_ctx) avcodec_free_context(&a_enc_ctx);
        if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
        if (a_in_fmt) avformat_close_input(&a_in_fmt);
        avcodec_free_context(&v_enc_ctx);
        avformat_free_context(out_fmt);
        return;
    }

    // Build the source pool for video. Open one decoder per unique
    // media_id used by clips in the trim range.
    std::map<std::string, std::unique_ptr<VideoSource>> sources;
    for (const auto& c : proj.clips()) {
        // Audio-only sub-track clips don't contribute video, so we
        // don't open a video decoder for them. Their picture stream
        // (if the source has one) would just waste seek time.
        if (c.track < 0)                 continue;
        if (c.t_in >= req.trim_end_us)   continue;
        if (c.t_out() <= req.trim_start_us) continue;
        if (sources.count(c.media_id))   continue;
        const Media* m = proj.find_media(c.media_id);
        if (!m) continue;
        auto src = std::make_unique<VideoSource>();
        if (src->open(narrow_path(m->path))) {
            sources[c.media_id] = std::move(src);
        }
    }

    // Allocate canvas + intermediate.
    AVFrame* canvas = av_frame_alloc();
    canvas->format = v_enc_ctx->pix_fmt;
    canvas->width  = out_w;
    canvas->height = out_h;
    av_frame_get_buffer(canvas, 32);

    AVPacket* enc_pkt = av_packet_alloc();
    auto encode_video_frame = [&](AVFrame* f) -> int {
        int rc = avcodec_send_frame(v_enc_ctx, f);
        if (rc < 0 && rc != AVERROR(EAGAIN)) return rc;
        while (true) {
            rc = avcodec_receive_packet(v_enc_ctx, enc_pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return rc;
            enc_pkt->stream_index = v_out->index;
            av_packet_rescale_ts(enc_pkt, v_enc_ctx->time_base,
                                 v_out->time_base);
            int wr = av_interleaved_write_frame(out_fmt, enc_pkt);
            av_packet_unref(enc_pkt);
            if (wr < 0) return wr;
        }
        return 0;
    };
    auto encode_audio_frame = [&](AVFrame* f) -> int {
        if (!a_enc_ctx) return 0;
        int rc = avcodec_send_frame(a_enc_ctx, f);
        if (rc < 0 && rc != AVERROR(EAGAIN)) return rc;
        while (true) {
            rc = avcodec_receive_packet(a_enc_ctx, enc_pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return rc;
            enc_pkt->stream_index = a_out_stream->index;
            av_packet_rescale_ts(enc_pkt, a_enc_ctx->time_base,
                                 a_out_stream->time_base);
            int wr = av_interleaved_write_frame(out_fmt, enc_pkt);
            av_packet_unref(enc_pkt);
            if (wr < 0) return wr;
        }
        return 0;
    };

    // Helper: composite one source frame into the canvas with the
    // clip's transform. Lazily caches a SwsContext per clip in a small
    // local map; rebuilds when sub-rect dimensions change (rarely).
    struct ClipSwsCache {
        SwsContext* sws = nullptr;
        int sx0 = 0, sy0 = 0, svw = 0, svh = 0;
        int vx0 = 0, vy0 = 0, vw  = 0, vh  = 0;
        ~ClipSwsCache() { if (sws) sws_freeContext(sws); }
    };
    std::map<std::string, ClipSwsCache> sws_cache;

    auto composite = [&](const Clip* c, AVFrame* src) {
        if (!c || !src) return;
        // Compute target rect from clip transform.
        const float xs = (c->scale > 0.f) ? c->scale : 1.0f;
        const int target_w = std::max(2, int(out_w * xs + 0.5f)) & ~1;
        const int target_h = std::max(2, int(out_h * xs + 0.5f)) & ~1;
        const int target_x = int(out_w * 0.5f + c->pos_x * out_w * 0.5f
                                 - target_w * 0.5f);
        const int target_y = int(out_h * 0.5f + c->pos_y * out_h * 0.5f
                                 - target_h * 0.5f);
        int vx0 = std::max(0, target_x) & ~1;
        int vy0 = std::max(0, target_y) & ~1;
        int vx1 = std::min(out_w, target_x + target_w) & ~1;
        int vy1 = std::min(out_h, target_y + target_h) & ~1;
        int vw  = std::max(2, vx1 - vx0);
        int vh  = std::max(2, vy1 - vy0);

        const int sw = src->width;
        const int sh = src->height;
        int sx0 = ((vx0 - target_x) * sw / target_w) & ~1;
        int sy0 = ((vy0 - target_y) * sh / target_h) & ~1;
        int sx1 = ((vx1 - target_x) * sw / target_w) & ~1;
        int sy1 = ((vy1 - target_y) * sh / target_h) & ~1;
        int svw = std::max(2, sx1 - sx0);
        int svh = std::max(2, sy1 - sy0);

        ClipSwsCache& sc = sws_cache[c->id];
        if (!sc.sws || sc.svw != svw || sc.svh != svh
                    || sc.vw  != vw  || sc.vh  != vh) {
            if (sc.sws) sws_freeContext(sc.sws);
            sc.sws = sws_getContext(svw, svh, (AVPixelFormat)src->format,
                                    vw,  vh,  v_enc_ctx->pix_fmt,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr);
            sc.svw = svw; sc.svh = svh;
            sc.vw  = vw;  sc.vh  = vh;
            sc.sx0 = sx0; sc.sy0 = sy0;
            sc.vx0 = vx0; sc.vy0 = vy0;
        }
        sc.sx0 = sx0; sc.sy0 = sy0;
        sc.vx0 = vx0; sc.vy0 = vy0;
        if (!sc.sws) return;
        uint8_t* src_data[4]; int src_ls[4];
        plane_off(src, sc.sx0, sc.sy0, src_data, src_ls);
        uint8_t* dst_data[4]; int dst_ls[4];
        plane_off(canvas, sc.vx0, sc.vy0, dst_data, dst_ls);
        sws_scale(sc.sws, src_data, src_ls, 0, sc.svh, dst_data, dst_ls);
    };

    // ----- Video render loop -----
    set_status("Encoding...");
    int64_t v_pts = 0;
    bool failed = false;
    for (int64_t fi = 0; fi < total_frames && !cancel.load() && !failed; ++fi) {
        const TimeUs tl_us = req.trim_start_us + fi * frame_us;
        const Clip* top = nullptr;
        const Clip* bot = nullptr;
        proj.clips_at(tl_us, &top, &bot);
        // clips_at() doesn't filter by visual vs audio-only — audio
        // sub-tracks live on negative track indices and don't carry
        // pictures. Drop them so the export's video stage doesn't try
        // to composite from a source we never opened.
        if (top && top->track < 0) top = nullptr;
        if (bot && bot->track < 0) bot = nullptr;

        canvas_black(canvas);
        if (bot) {
            auto it = sources.find(bot->media_id);
            if (it != sources.end()) {
                const TimeUs offset = tl_us - bot->t_in;
                const TimeUs file_pts = bot->src_in
                    + TimeUs(double(offset) * bot->speed);
                AVFrame* f = it->second->frame_at(file_pts);
                composite(bot, f);
            }
        }
        if (top) {
            auto it = sources.find(top->media_id);
            if (it != sources.end()) {
                const TimeUs offset = tl_us - top->t_in;
                const TimeUs file_pts = top->src_in
                    + TimeUs(double(offset) * top->speed);
                AVFrame* f = it->second->frame_at(file_pts);
                composite(top, f);
            }
        }
        canvas->pts = v_pts++;
        if (encode_video_frame(canvas) < 0) {
            failed = true;
            break;
        }
        progress.store(float(double(fi + 1) / double(total_frames) * 0.9));
    }
    encode_video_frame(nullptr);  // flush

    // ----- Audio render -----
    if (!failed && a_dec_ctx && a_enc_ctx && audio_clip) {
        // Seek to audio clip's source-time corresponding to trim_start.
        const TimeUs offset0 = std::max<TimeUs>(0,
            req.trim_start_us - audio_clip->t_in);
        const TimeUs aud_seek_us = audio_clip->src_in
            + TimeUs(double(offset0) * audio_clip->speed);
        const TimeUs aud_end_us = audio_clip->src_in
            + TimeUs(double(req.trim_end_us - audio_clip->t_in)
                     * audio_clip->speed);
        int64_t seek_ts = av_rescale_q(aud_seek_us,
                                       AVRational{1, 1'000'000},
                                       a_in_stream->time_base);
        av_seek_frame(a_in_fmt, a_in_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(a_dec_ctx);

        AVPacket* a_pkt = av_packet_alloc();
        AVFrame*  a_dec_f = av_frame_alloc();
        int64_t a_pts_count = 0;
        bool aud_eof = false;
        while (!cancel.load() && !aud_eof) {
            int rc = av_read_frame(a_in_fmt, a_pkt);
            if (rc < 0) break;
            if (a_pkt->stream_index != a_in_idx) {
                av_packet_unref(a_pkt);
                continue;
            }
            avcodec_send_packet(a_dec_ctx, a_pkt);
            av_packet_unref(a_pkt);
            while (true) {
                rc = avcodec_receive_frame(a_dec_ctx, a_dec_f);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                if (rc < 0) { aud_eof = true; break; }
                int64_t pts_ts = (a_dec_f->best_effort_timestamp != AV_NOPTS_VALUE)
                    ? a_dec_f->best_effort_timestamp : a_dec_f->pts;
                int64_t pts_us = av_rescale_q(pts_ts, a_in_stream->time_base,
                                              AVRational{1, 1'000'000});
                if (pts_us > aud_end_us) {
                    aud_eof = true;
                    av_frame_unref(a_dec_f);
                    break;
                }
                int max_out = int(av_rescale_rnd(
                    swr_get_delay(swr, a_dec_ctx->sample_rate)
                        + a_dec_f->nb_samples,
                    a_enc_ctx->sample_rate,
                    a_dec_ctx->sample_rate, AV_ROUND_UP));
                AVFrame* af = av_frame_alloc();
                af->format     = a_enc_ctx->sample_fmt;
                af->sample_rate= a_enc_ctx->sample_rate;
                af->nb_samples = max_out;
                av_channel_layout_copy(&af->ch_layout, &a_enc_ctx->ch_layout);
                av_frame_get_buffer(af, 0);
                int got = swr_convert(swr, af->data, max_out,
                    (const uint8_t**)a_dec_f->extended_data,
                    a_dec_f->nb_samples);
                if (got > 0) {
                    af->nb_samples = got;
                    af->pts = a_pts_count;
                    a_pts_count += got;
                    encode_audio_frame(af);
                }
                av_frame_free(&af);
                av_frame_unref(a_dec_f);
            }
        }
        encode_audio_frame(nullptr);
        av_packet_free(&a_pkt);
        av_frame_free(&a_dec_f);
    }

    av_write_trailer(out_fmt);

    av_packet_free(&enc_pkt);
    av_frame_free(&canvas);
    if (out_fmt->pb) avio_closep(&out_fmt->pb);
    if (swr) swr_free(&swr);
    if (a_enc_ctx) avcodec_free_context(&a_enc_ctx);
    avcodec_free_context(&v_enc_ctx);
    if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
    if (a_in_fmt) avformat_close_input(&a_in_fmt);
    avformat_free_context(out_fmt);
    sources.clear();
    sws_cache.clear();

    if (!failed && !cancel.load()) {
        progress.store(1.0f);
        success.store(true);
        set_status("Project export done.");
    } else {
        set_status(cancel.load() ? "Cancelled." : "Project export failed.");
    }
}

void Exporter::run_one_export(ExportRequest req) {
    auto set_status = [&](std::string s) {
        std::lock_guard lk(mu_);
        status_ = std::move(s);
    };

    if (req.audio_only) {
        run_audio_only_export(req, progress_, cancel_, success_, set_status);
        return;
    }

    // Multi-track / multi-source compositing path. Routed when the
    // project has more than one clip OR has any clip on a non-zero
    // track. Single-clip projects fall through to the original
    // single-source pipeline (which is faster and preserves audio
    // bit-exactness).
    if (req.project) {
        const auto& clips = req.project->clips();
        bool needs_project = clips.size() > 1;
        if (!needs_project) {
            for (const auto& c : clips) {
                // Any non-V0 clip (overlay or audio sub-track) needs
                // the multi-source compositing path.
                if (c.track != 0) { needs_project = true; break; }
            }
        }
        if (needs_project) {
            run_project_export(req, progress_, cancel_, success_, set_status);
            return;
        }
    }

    set_status("Opening source...");

    // ------------------------------------------------------------------
    // Input demuxer + decoders.
    // ------------------------------------------------------------------
    AVFormatContext* in_fmt = nullptr;
    int err = avformat_open_input(&in_fmt, narrow_path(req.source_path).c_str(),
                                  nullptr, nullptr);
    if (err < 0) {
        set_status("Cannot open source: " + averr(err));
        return;
    }
    err = avformat_find_stream_info(in_fmt, nullptr);
    if (err < 0) {
        set_status("Cannot probe streams: " + averr(err));
        avformat_close_input(&in_fmt);
        return;
    }

    int v_in_idx = av_find_best_stream(in_fmt, AVMEDIA_TYPE_VIDEO,
                                       -1, -1, nullptr, 0);
    int a_in_idx = av_find_best_stream(in_fmt, AVMEDIA_TYPE_AUDIO,
                                       -1, -1, nullptr, 0);
    if (v_in_idx < 0) {
        set_status("Source has no video stream");
        avformat_close_input(&in_fmt);
        return;
    }

    AVStream* v_in = in_fmt->streams[v_in_idx];
    AVStream* a_in = (a_in_idx >= 0) ? in_fmt->streams[a_in_idx] : nullptr;

    // Video decoder.
    const AVCodec* v_dec = avcodec_find_decoder(v_in->codecpar->codec_id);
    if (!v_dec) {
        set_status("No decoder for input video codec");
        avformat_close_input(&in_fmt);
        return;
    }
    AVCodecContext* v_dec_ctx = avcodec_alloc_context3(v_dec);
    avcodec_parameters_to_context(v_dec_ctx, v_in->codecpar);
    v_dec_ctx->thread_count = 0;
    err = avcodec_open2(v_dec_ctx, v_dec, nullptr);
    if (err < 0) {
        set_status("Open input video decoder failed: " + averr(err));
        avcodec_free_context(&v_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }

    // Audio decoder.
    AVCodecContext* a_dec_ctx = nullptr;
    if (a_in) {
        const AVCodec* a_dec = avcodec_find_decoder(a_in->codecpar->codec_id);
        if (a_dec) {
            a_dec_ctx = avcodec_alloc_context3(a_dec);
            avcodec_parameters_to_context(a_dec_ctx, a_in->codecpar);
            err = avcodec_open2(a_dec_ctx, a_dec, nullptr);
            if (err < 0) {
                avcodec_free_context(&a_dec_ctx);
                a_dec_ctx = nullptr;
                a_in = nullptr;
            }
        } else {
            a_in = nullptr;
        }
    }

    // ------------------------------------------------------------------
    // Output muxer + encoders.
    // ------------------------------------------------------------------
    set_status("Opening output...");

    const std::string out_path = narrow_path(req.output_path);
    AVFormatContext* out_fmt = nullptr;
    err = avformat_alloc_output_context2(&out_fmt, nullptr, "mp4",
                                         out_path.c_str());
    if (err < 0 || !out_fmt) {
        set_status("Cannot create output: " + averr(err));
        avcodec_free_context(&v_dec_ctx);
        if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }

    // Pick the video encoder.
    const char* h264_hw[] = { "h264_nvenc", "h264_qsv", "h264_amf", "h264_mf" };
    const char* h264_sw[] = { "libopenh264", "h264" };
    const char* hevc_hw[] = { "hevc_nvenc", "hevc_qsv", "hevc_amf", "hevc_mf" };
    const char* hevc_sw[] = { "hevc", "libx265" };

    const AVCodec* v_enc = nullptr;
    if (req.codec == ExportCodec::HEVC) {
        if (req.hardware) v_enc = find_encoder(hevc_hw, sizeof(hevc_hw)/sizeof(*hevc_hw));
        if (!v_enc)       v_enc = find_encoder(hevc_sw, sizeof(hevc_sw)/sizeof(*hevc_sw));
        if (!v_enc)       v_enc = find_encoder(hevc_hw, sizeof(hevc_hw)/sizeof(*hevc_hw));
    } else {
        if (req.hardware) v_enc = find_encoder(h264_hw, sizeof(h264_hw)/sizeof(*h264_hw));
        if (!v_enc)       v_enc = find_encoder(h264_sw, sizeof(h264_sw)/sizeof(*h264_sw));
        if (!v_enc)       v_enc = find_encoder(h264_hw, sizeof(h264_hw)/sizeof(*h264_hw));
    }
    if (!v_enc) {
        set_status(req.codec == ExportCodec::HEVC
            ? "No HEVC encoder available in this libavcodec build"
            : "No H.264 encoder available in this libavcodec build");
        avformat_free_context(out_fmt);
        avcodec_free_context(&v_dec_ctx);
        if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }

    AVCodecContext* v_enc_ctx = avcodec_alloc_context3(v_enc);

    // Aspect-ratio crop (matches the viewer's "AR:" combo). When the
    // user picks a non-source AR, we centre-crop the source frame to
    // that ratio before scaling to the encoder. We compute the crop
    // rectangle here so it can also constrain the output resolution.
    const int src_w = v_dec_ctx->width;
    const int src_h = v_dec_ctx->height;
    int crop_w = src_w, crop_h = src_h, crop_x = 0, crop_y = 0;
    if (req.aspect_ratio > 0.0) {
        const double src_ar = double(src_w) / double(std::max(1, src_h));
        if (req.aspect_ratio > src_ar) {
            // Output is wider than source: crop top + bottom.
            crop_h = int(double(src_w) / req.aspect_ratio + 0.5);
            crop_h &= ~1;
            crop_y = ((src_h - crop_h) / 2) & ~1;
        } else if (req.aspect_ratio < src_ar) {
            // Output is taller: crop left + right.
            crop_w = int(double(src_h) * req.aspect_ratio + 0.5);
            crop_w &= ~1;
            crop_x = ((src_w - crop_w) / 2) & ~1;
        }
    }

    int out_w = (req.width  > 0) ? req.width  : crop_w;
    int out_h = (req.height > 0) ? req.height : crop_h;
    if (req.aspect_ratio > 0.0 && req.width == 0 && req.height == 0) {
        // No explicit resolution + AR override: use the cropped size.
        out_w = crop_w;
        out_h = crop_h;
    } else if (req.aspect_ratio > 0.0 && (req.width > 0 || req.height > 0)) {
        // User pinned a resolution AND wants an AR override. Snap
        // out_h to match req.aspect_ratio so the encoded image
        // doesn't get stretched.
        if (req.width > 0 && req.height == 0) {
            out_h = int(double(req.width) / req.aspect_ratio + 0.5);
        } else if (req.height > 0 && req.width == 0) {
            out_w = int(double(req.height) * req.aspect_ratio + 0.5);
        }
    }
    out_w &= ~1;          // even dimensions for h264/hevc
    out_h &= ~1;
    v_enc_ctx->width  = out_w;
    v_enc_ctx->height = out_h;
    v_enc_ctx->bit_rate = req.video_bitrate;
    v_enc_ctx->pix_fmt  = AV_PIX_FMT_YUV420P;
    AVRational out_tb;
    if (req.fps_num > 0) {
        out_tb = AVRational{req.fps_den, req.fps_num};
    } else if (av_guess_frame_rate(in_fmt, v_in, nullptr).num > 0) {
        AVRational fr = av_guess_frame_rate(in_fmt, v_in, nullptr);
        out_tb = AVRational{fr.den, fr.num};
    } else {
        out_tb = AVRational{1, 30};
    }
    v_enc_ctx->time_base  = out_tb;
    v_enc_ctx->framerate  = AVRational{out_tb.den, out_tb.num};
    v_enc_ctx->gop_size   = std::max(1, v_enc_ctx->framerate.num * 2);
    v_enc_ctx->max_b_frames = 2;
    v_enc_ctx->sample_aspect_ratio = AVRational{1, 1};
    if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
        v_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    // NVENC / QSV / AMF prefer NV12 input; the rest take YUV420P.
    if (std::string(v_enc->name).find("nvenc") != std::string::npos
     || std::string(v_enc->name).find("amf")   != std::string::npos
     || std::string(v_enc->name).find("qsv")   != std::string::npos) {
        v_enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    }

    AVDictionary* enc_opts = nullptr;
    if (std::string(v_enc->name).find("nvenc") != std::string::npos) {
        av_dict_set(&enc_opts, "preset", "p4", 0);
        av_dict_set(&enc_opts, "tune",   "hq", 0);
    }
    err = avcodec_open2(v_enc_ctx, v_enc, &enc_opts);
    av_dict_free(&enc_opts);
    if (err < 0) {
        set_status(std::string("Open ") + v_enc->name +
                   " encoder failed: " + averr(err));
        avcodec_free_context(&v_enc_ctx);
        avformat_free_context(out_fmt);
        avcodec_free_context(&v_dec_ctx);
        if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }
    log::info("Exporter: encoder = %s, %dx%d (src %dx%d crop %dx%d+%d+%d), "
              "%d kbps, %d/%d fps, pix=%s",
              v_enc->name, out_w, out_h,
              src_w, src_h, crop_w, crop_h, crop_x, crop_y,
              req.video_bitrate / 1000,
              v_enc_ctx->framerate.num, v_enc_ctx->framerate.den,
              av_get_pix_fmt_name(v_enc_ctx->pix_fmt));

    AVStream* v_out = avformat_new_stream(out_fmt, nullptr);
    if (!v_out) {
        set_status("avformat_new_stream(video) failed");
        avcodec_free_context(&v_enc_ctx);
        avformat_free_context(out_fmt);
        avcodec_free_context(&v_dec_ctx);
        if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }
    avcodec_parameters_from_context(v_out->codecpar, v_enc_ctx);
    v_out->time_base = v_enc_ctx->time_base;

    // Audio encoder.
    const AVCodec*    a_enc     = nullptr;
    AVCodecContext*   a_enc_ctx = nullptr;
    AVStream*         a_out     = nullptr;
    SwrContext*       swr       = nullptr;
    if (a_dec_ctx) {
        a_enc = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (a_enc) {
            a_enc_ctx = avcodec_alloc_context3(a_enc);
            a_enc_ctx->sample_rate = 48000;
            a_enc_ctx->bit_rate    = req.audio_bitrate;
            a_enc_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
            av_channel_layout_default(&a_enc_ctx->ch_layout, 2);
            a_enc_ctx->time_base   = AVRational{1, a_enc_ctx->sample_rate};
            if (out_fmt->oformat->flags & AVFMT_GLOBALHEADER) {
                a_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            }
            err = avcodec_open2(a_enc_ctx, a_enc, nullptr);
            if (err < 0) {
                log::warn("Exporter: AAC encoder open failed: %s",
                          averr(err).c_str());
                avcodec_free_context(&a_enc_ctx);
                a_enc_ctx = nullptr;
            } else {
                a_out = avformat_new_stream(out_fmt, nullptr);
                avcodec_parameters_from_context(a_out->codecpar, a_enc_ctx);
                a_out->time_base = a_enc_ctx->time_base;

                // Set up resampler from decoder format -> encoder
                // input format.
                AVChannelLayout in_layout;
                if (a_dec_ctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC) {
                    av_channel_layout_copy(&in_layout, &a_dec_ctx->ch_layout);
                } else {
                    av_channel_layout_default(&in_layout,
                        a_dec_ctx->ch_layout.nb_channels);
                }
                AVChannelLayout out_layout;
                av_channel_layout_default(&out_layout, 2);
                err = swr_alloc_set_opts2(&swr,
                    &out_layout, a_enc_ctx->sample_fmt, a_enc_ctx->sample_rate,
                    &in_layout,  a_dec_ctx->sample_fmt, a_dec_ctx->sample_rate,
                    0, nullptr);
                if (err < 0 || !swr || swr_init(swr) < 0) {
                    if (swr) swr_free(&swr);
                    avcodec_free_context(&a_enc_ctx);
                    a_enc_ctx = nullptr;
                    a_out = nullptr;
                }
                av_channel_layout_uninit(&in_layout);
                av_channel_layout_uninit(&out_layout);
            }
        }
    }

    // ------------------------------------------------------------------
    // Open output file & write header.
    // ------------------------------------------------------------------
    if (!(out_fmt->oformat->flags & AVFMT_NOFILE)) {
        err = avio_open(&out_fmt->pb, out_path.c_str(), AVIO_FLAG_WRITE);
        if (err < 0) {
            set_status("avio_open failed: " + averr(err));
            if (swr) swr_free(&swr);
            if (a_enc_ctx) avcodec_free_context(&a_enc_ctx);
            avcodec_free_context(&v_enc_ctx);
            avformat_free_context(out_fmt);
            avcodec_free_context(&v_dec_ctx);
            if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
            avformat_close_input(&in_fmt);
            return;
        }
    }
    err = avformat_write_header(out_fmt, nullptr);
    if (err < 0) {
        set_status("write_header failed: " + averr(err));
        if (out_fmt->pb) avio_closep(&out_fmt->pb);
        if (swr) swr_free(&swr);
        if (a_enc_ctx) avcodec_free_context(&a_enc_ctx);
        avcodec_free_context(&v_enc_ctx);
        avformat_free_context(out_fmt);
        avcodec_free_context(&v_dec_ctx);
        if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
        avformat_close_input(&in_fmt);
        return;
    }

    // Seek to trim start.
    if (req.trim_start_us > 0) {
        int64_t seek_ts = av_rescale_q(req.trim_start_us,
                                       AVRational{1, 1'000'000},
                                       v_in->time_base);
        av_seek_frame(in_fmt, v_in_idx, seek_ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(v_dec_ctx);
        if (a_dec_ctx) avcodec_flush_buffers(a_dec_ctx);
    }

    // Probe duration for progress.
    int64_t total_us = 0;
    if (in_fmt->duration != AV_NOPTS_VALUE) {
        total_us = av_rescale_q(in_fmt->duration, AVRational{1, AV_TIME_BASE},
                                AVRational{1, 1'000'000});
    }
    if (req.trim_end_us < 0 || req.trim_end_us > total_us) {
        req.trim_end_us = total_us;
    }
    const int64_t span_us = std::max<int64_t>(
        1, int64_t(req.trim_end_us) - int64_t(req.trim_start_us));

    // ------------------------------------------------------------------
    // Encode loop.
    // ------------------------------------------------------------------
    set_status("Encoding...");

    // Per-clip transform inside the canvas. We mirror the viewer's
    // model: scale grows/shrinks the picture, pos_x/pos_y in [-1..1]
    // pan it in normalised half-frame units. Outside the picture is
    // canvas black. When the transform is identity, we degenerate to
    // the previous full-frame sws_scale.
    const float xs = (req.xform_scale > 0.f) ? req.xform_scale : 1.0f;
    const float xx = req.xform_pos_x;
    const float xy = req.xform_pos_y;
    const bool  has_xform = (xs != 1.0f) || (xx != 0.0f) || (xy != 0.0f);

    int target_w = out_w, target_h = out_h, target_x = 0, target_y = 0;
    int vx0 = 0, vy0 = 0, vw = out_w, vh = out_h;
    int sx0 = crop_x, sy0 = crop_y, svw = crop_w, svh = crop_h;
    if (has_xform) {
        target_w = std::max(2, int(out_w * xs + 0.5f)) & ~1;
        target_h = std::max(2, int(out_h * xs + 0.5f)) & ~1;
        target_x = int(out_w * 0.5f + xx * out_w * 0.5f - target_w * 0.5f);
        target_y = int(out_h * 0.5f + xy * out_h * 0.5f - target_h * 0.5f);

        vx0 = std::max(0, target_x) & ~1;
        vy0 = std::max(0, target_y) & ~1;
        int vx1 = std::min(out_w, target_x + target_w) & ~1;
        int vy1 = std::min(out_h, target_y + target_h) & ~1;
        vw = std::max(2, vx1 - vx0);
        vh = std::max(2, vy1 - vy0);

        // Map the visible canvas rect back into the source crop rect.
        sx0 = (crop_x + (vx0 - target_x) * crop_w / target_w) & ~1;
        sy0 = (crop_y + (vy0 - target_y) * crop_h / target_h) & ~1;
        int sx1 = (crop_x + (vx1 - target_x) * crop_w / target_w) & ~1;
        int sy1 = (crop_y + (vy1 - target_y) * crop_h / target_h) & ~1;
        svw = std::max(2, sx1 - sx0);
        svh = std::max(2, sy1 - sy0);
    }

    // sws_scale runs on the visible source sub-region of every
    // decoded frame. Per-frame we offset dec_frame->data[] pointers
    // using the (sx0,sy0) origin computed above; linesize is
    // unchanged.
    SwsContext* sws = sws_getContext(
        svw, svh, v_dec_ctx->pix_fmt,
        vw,  vh,  v_enc_ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    // Helper: compute offset data pointers into a planar / semi-
    // planar frame at (off_x, off_y). Used for both the source-side
    // crop and the destination-side transform offset.
    auto plane_offsets = [&](const AVFrame* f, int off_x, int off_y,
                             uint8_t* out_data[4], int out_ls[4]) {
        const AVPixelFormat fmt = (AVPixelFormat)f->format;
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
        int bps[4]  = {1, 1, 1, 1};
        int hsub[4] = {0, 0, 0, 0};
        int vsub[4] = {0, 0, 0, 0};
        bool has[4] = {false, false, false, false};
        if (d) {
            for (int c = 0; c < d->nb_components; ++c) {
                int p = d->comp[c].plane;
                if (p < 0 || p >= 4) continue;
                bps[p] = std::max(bps[p], int(d->comp[c].step));
                if (c == 1 || c == 2) {
                    hsub[p] = d->log2_chroma_w;
                    vsub[p] = d->log2_chroma_h;
                }
                has[p] = true;
            }
        }
        for (int p = 0; p < 4; ++p) {
            out_ls[p] = f->linesize[p];
            if (!f->data[p] || !has[p]) {
                out_data[p] = f->data[p];
                continue;
            }
            out_data[p] = f->data[p]
                + (off_y >> vsub[p]) * f->linesize[p]
                + (off_x >> hsub[p]) * bps[p];
        }
    };

    auto crop_offsets = [&](const AVFrame* f,
                            uint8_t* out_data[4], int out_ls[4]) {
        plane_offsets(f, sx0, sy0, out_data, out_ls);
    };

    // Black-fill the encoder canvas. For YUV420P / YUV422P / YUV444P
    // we set Y=0 and chroma=128 (neutral gray). For NV12 the chroma
    // plane is interleaved UV at the same neutral.
    auto fill_canvas_black = [&](AVFrame* f) {
        const AVPixelFormat fmt = (AVPixelFormat)f->format;
        if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUV422P
         || fmt == AV_PIX_FMT_YUV444P) {
            std::memset(f->data[0], 0,   f->linesize[0] * f->height);
            std::memset(f->data[1], 128, f->linesize[1] * f->height);
            std::memset(f->data[2], 128, f->linesize[2] * f->height);
        } else if (fmt == AV_PIX_FMT_NV12) {
            std::memset(f->data[0], 0,   f->linesize[0] * f->height);
            std::memset(f->data[1], 128, f->linesize[1] * f->height);
        }
    };

    AVPacket* pkt        = av_packet_alloc();
    AVPacket* enc_pkt    = av_packet_alloc();
    AVFrame*  dec_frame  = av_frame_alloc();
    AVFrame*  enc_frame  = av_frame_alloc();
    AVFrame*  audio_frame= av_frame_alloc();

    enc_frame->format = v_enc_ctx->pix_fmt;
    enc_frame->width  = out_w;
    enc_frame->height = out_h;
    av_frame_get_buffer(enc_frame, 32);

    int64_t v_pts_count = 0;
    int64_t a_pts_count = 0;
    int     a_frame_size = (a_enc_ctx ? a_enc_ctx->frame_size : 1024);

    auto encode_video_frame = [&](AVFrame* f) -> int {
        int rc = avcodec_send_frame(v_enc_ctx, f);
        if (rc < 0 && rc != AVERROR(EAGAIN)) {
            log::warn("Exporter: send video frame: %s", averr(rc).c_str());
            return rc;
        }
        while (true) {
            rc = avcodec_receive_packet(v_enc_ctx, enc_pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return rc;
            enc_pkt->stream_index = v_out->index;
            av_packet_rescale_ts(enc_pkt, v_enc_ctx->time_base,
                                 v_out->time_base);
            int wr = av_interleaved_write_frame(out_fmt, enc_pkt);
            av_packet_unref(enc_pkt);
            if (wr < 0) return wr;
        }
        return 0;
    };

    auto encode_audio_frame = [&](AVFrame* f) -> int {
        if (!a_enc_ctx) return 0;
        int rc = avcodec_send_frame(a_enc_ctx, f);
        if (rc < 0 && rc != AVERROR(EAGAIN)) return rc;
        while (true) {
            rc = avcodec_receive_packet(a_enc_ctx, enc_pkt);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return rc;
            enc_pkt->stream_index = a_out->index;
            av_packet_rescale_ts(enc_pkt, a_enc_ctx->time_base,
                                 a_out->time_base);
            int wr = av_interleaved_write_frame(out_fmt, enc_pkt);
            av_packet_unref(enc_pkt);
            if (wr < 0) return wr;
        }
        return 0;
    };

    bool failed = false;
    bool video_eof = false;
    bool audio_eof = (a_enc_ctx == nullptr);

    while (!cancel_.load() && (!video_eof || !audio_eof)) {
        err = av_read_frame(in_fmt, pkt);
        if (err < 0) break;

        if (pkt->stream_index == v_in_idx && !video_eof) {
            int rc = avcodec_send_packet(v_dec_ctx, pkt);
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                av_packet_unref(pkt);
                continue;
            }
            while (true) {
                rc = avcodec_receive_frame(v_dec_ctx, dec_frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                if (rc < 0) { failed = true; break; }

                int64_t pts_ts = (dec_frame->best_effort_timestamp != AV_NOPTS_VALUE)
                    ? dec_frame->best_effort_timestamp
                    : dec_frame->pts;
                int64_t pts_us = av_rescale_q(pts_ts, v_in->time_base,
                                              AVRational{1, 1'000'000});
                if (pts_us > req.trim_end_us) {
                    video_eof = true;
                    av_frame_unref(dec_frame);
                    break;
                }
                progress_.store(float(double(pts_us - req.trim_start_us) /
                                      double(span_us)));

                // Crop + convert + scale + transform into the encoder
                // canvas. When transform is identity the destination
                // covers the whole canvas; otherwise we black-fill
                // the canvas first then sws_scale into a sub-rect at
                // (vx0, vy0).
                if (sws) {
                    uint8_t* src_data[4];
                    int      src_ls[4];
                    crop_offsets(dec_frame, src_data, src_ls);
                    if (has_xform) {
                        fill_canvas_black(enc_frame);
                        uint8_t* dst_data[4];
                        int      dst_ls[4];
                        plane_offsets(enc_frame, vx0, vy0,
                                      dst_data, dst_ls);
                        sws_scale(sws, src_data, src_ls, 0, svh,
                                  dst_data, dst_ls);
                    } else {
                        sws_scale(sws, src_data, src_ls, 0, svh,
                                  enc_frame->data, enc_frame->linesize);
                    }
                }
                enc_frame->pts = v_pts_count++;
                int rc2 = encode_video_frame(enc_frame);
                if (rc2 < 0) failed = true;
                av_frame_unref(dec_frame);
            }
        } else if (a_dec_ctx && pkt->stream_index == a_in_idx && !audio_eof) {
            int rc = avcodec_send_packet(a_dec_ctx, pkt);
            if (rc < 0 && rc != AVERROR(EAGAIN)) {
                av_packet_unref(pkt);
                continue;
            }
            while (true) {
                rc = avcodec_receive_frame(a_dec_ctx, dec_frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                if (rc < 0) { failed = true; break; }

                int64_t pts_ts = (dec_frame->best_effort_timestamp != AV_NOPTS_VALUE)
                    ? dec_frame->best_effort_timestamp
                    : dec_frame->pts;
                int64_t pts_us = av_rescale_q(pts_ts, a_in->time_base,
                                              AVRational{1, 1'000'000});
                if (pts_us > req.trim_end_us) {
                    audio_eof = true;
                    av_frame_unref(dec_frame);
                    break;
                }

                // swr converts directly into a fresh frame sized to
                // the encoder's frame_size.
                int max_out = int(av_rescale_rnd(
                    swr_get_delay(swr, a_dec_ctx->sample_rate)
                        + dec_frame->nb_samples,
                    a_enc_ctx->sample_rate,
                    a_dec_ctx->sample_rate, AV_ROUND_UP));

                AVFrame* af = av_frame_alloc();
                af->format         = a_enc_ctx->sample_fmt;
                af->sample_rate    = a_enc_ctx->sample_rate;
                af->nb_samples     = max_out;
                av_channel_layout_copy(&af->ch_layout, &a_enc_ctx->ch_layout);
                av_frame_get_buffer(af, 0);
                int got = swr_convert(swr,
                    af->data, max_out,
                    (const uint8_t**)dec_frame->extended_data,
                    dec_frame->nb_samples);
                if (got > 0) {
                    af->nb_samples = got;
                    af->pts        = a_pts_count;
                    a_pts_count   += got;
                    int rc2 = encode_audio_frame(af);
                    if (rc2 < 0) failed = true;
                }
                av_frame_free(&af);
                av_frame_unref(dec_frame);
            }
        }
        av_packet_unref(pkt);
        if (failed) break;
    }

    // Flush decoders.
    if (!cancel_.load() && !failed) {
        avcodec_send_packet(v_dec_ctx, nullptr);
        while (true) {
            int rc = avcodec_receive_frame(v_dec_ctx, dec_frame);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) break;
            if (sws) {
                uint8_t* src_data[4];
                int      src_ls[4];
                crop_offsets(dec_frame, src_data, src_ls);
                if (has_xform) {
                    fill_canvas_black(enc_frame);
                    uint8_t* dst_data[4];
                    int      dst_ls[4];
                    plane_offsets(enc_frame, vx0, vy0,
                                  dst_data, dst_ls);
                    sws_scale(sws, src_data, src_ls, 0, svh,
                              dst_data, dst_ls);
                } else {
                    sws_scale(sws, src_data, src_ls, 0, svh,
                              enc_frame->data, enc_frame->linesize);
                }
            }
            enc_frame->pts = v_pts_count++;
            encode_video_frame(enc_frame);
            av_frame_unref(dec_frame);
        }
        if (a_dec_ctx) {
            avcodec_send_packet(a_dec_ctx, nullptr);
            while (true) {
                int rc = avcodec_receive_frame(a_dec_ctx, dec_frame);
                if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
                if (rc < 0) break;
                int max_out = int(av_rescale_rnd(
                    swr_get_delay(swr, a_dec_ctx->sample_rate)
                        + dec_frame->nb_samples,
                    a_enc_ctx->sample_rate,
                    a_dec_ctx->sample_rate, AV_ROUND_UP));
                AVFrame* af = av_frame_alloc();
                af->format      = a_enc_ctx->sample_fmt;
                af->sample_rate = a_enc_ctx->sample_rate;
                af->nb_samples  = max_out;
                av_channel_layout_copy(&af->ch_layout, &a_enc_ctx->ch_layout);
                av_frame_get_buffer(af, 0);
                int got = swr_convert(swr, af->data, max_out,
                    (const uint8_t**)dec_frame->extended_data,
                    dec_frame->nb_samples);
                if (got > 0) {
                    af->nb_samples = got;
                    af->pts        = a_pts_count;
                    a_pts_count   += got;
                    encode_audio_frame(af);
                }
                av_frame_free(&af);
                av_frame_unref(dec_frame);
            }
        }
    }

    // Flush encoders.
    encode_video_frame(nullptr);
    if (a_enc_ctx) encode_audio_frame(nullptr);

    if (!failed && !cancel_.load()) {
        av_write_trailer(out_fmt);
    }

    // Cleanup.
    if (sws) sws_freeContext(sws);
    av_packet_free(&pkt);
    av_packet_free(&enc_pkt);
    av_frame_free(&dec_frame);
    av_frame_free(&enc_frame);
    av_frame_free(&audio_frame);
    if (swr) swr_free(&swr);
    if (a_enc_ctx) avcodec_free_context(&a_enc_ctx);
    avcodec_free_context(&v_enc_ctx);
    if (out_fmt && out_fmt->pb) avio_closep(&out_fmt->pb);
    if (out_fmt) avformat_free_context(out_fmt);
    avcodec_free_context(&v_dec_ctx);
    if (a_dec_ctx) avcodec_free_context(&a_dec_ctx);
    avformat_close_input(&in_fmt);

    if (cancel_.load()) {
        set_status("Cancelled");
        success_.store(false);
    } else if (failed) {
        set_status("Encoding failed");
        success_.store(false);
    } else {
        progress_.store(1.0f);
        set_status("Done");
        success_.store(true);
    }
}

}  // namespace volchay::media
