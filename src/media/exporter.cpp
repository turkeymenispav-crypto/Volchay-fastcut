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
#include <chrono>
#include <cstring>
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

void Exporter::run_one_export(ExportRequest req) {
    auto set_status = [&](std::string s) {
        std::lock_guard lk(mu_);
        status_ = std::move(s);
    };

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

    // sws_scale runs on the cropped sub-region of every decoded
    // frame. Per-frame we offset dec_frame->data[] pointers using
    // the crop_x/crop_y origin computed above; the linesize stays the
    // same as the source.
    SwsContext* sws = sws_getContext(
        crop_w, crop_h, v_dec_ctx->pix_fmt,
        out_w, out_h, v_enc_ctx->pix_fmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);

    // Helper: compute offset src_data pointers into a decoded frame
    // for the centre-crop window. Linesize is unchanged. Works for
    // any planar / semi-planar format (YUV420P, NV12, YUV422P, ...).
    auto crop_offsets = [&](const AVFrame* f,
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
                + (crop_y >> vsub[p]) * f->linesize[p]
                + (crop_x >> hsub[p]) * bps[p];
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

                // Crop + convert + scale to encoder pix_fmt. When the
                // user picked a non-source AR, src_data points into
                // the centre-crop region of dec_frame; otherwise it's
                // the full frame.
                if (sws) {
                    uint8_t* src_data[4];
                    int      src_ls[4];
                    crop_offsets(dec_frame, src_data, src_ls);
                    sws_scale(sws, src_data, src_ls, 0, crop_h,
                              enc_frame->data, enc_frame->linesize);
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
                sws_scale(sws, src_data, src_ls, 0, crop_h,
                          enc_frame->data, enc_frame->linesize);
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
