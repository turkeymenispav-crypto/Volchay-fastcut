#include "ui/thumbnail_cache.h"

#include "util/log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>

namespace volchay::ui {
namespace {

// Default thumbnail size. Wide-screen 16:9, fits the Library's narrow
// left-dock column without scaling on the GPU side.
constexpr int kThumbW = 192;
constexpr int kThumbH = 108;

std::string narrow_path(const std::wstring& w) {
    int sz = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(sz, 0);
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()),
                          out.data(), sz, nullptr, nullptr);
    return out;
}

}  // namespace

ThumbnailCache::ThumbnailCache() {
    worker_ = std::thread(&ThumbnailCache::worker_main, this);
}

ThumbnailCache::~ThumbnailCache() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_.store(true);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void ThumbnailCache::set_device(ID3D11Device* device) {
    std::lock_guard<std::mutex> lk(mu_);
    device_ = device;
}

const ThumbnailCache::Entry* ThumbnailCache::get(const std::wstring& path) {
    std::lock_guard<std::mutex> lk(mu_);

    // Drain any decoded thumbnails ready for upload before we look up.
    while (!ready_.empty()) {
        Decoded d = std::move(ready_.front());
        ready_.pop_front();
        // Upload outside the lock to avoid keeping it during D3D calls.
        // But we also need to mutate entries_, so we do it under the lock —
        // D3D11Device::CreateTexture2D is thread-safe and won't deadlock.
        upload_decoded(d);
    }

    auto it = entries_.find(path);
    if (it == entries_.end()) {
        Entry e;
        entries_.emplace(path, std::move(e));
        order_.push_back(path);
        queue_.push_back(path);
        cv_.notify_one();
        it = entries_.find(path);
    }
    return &it->second;
}

void ThumbnailCache::for_each(
    const std::function<void(const std::wstring&,
                             const Entry&)>& fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : order_) {
        auto it = entries_.find(p);
        if (it != entries_.end()) fn(p, it->second);
    }
}

void ThumbnailCache::upload_decoded(const Decoded& d) {
    auto it = entries_.find(d.path);
    if (it == entries_.end()) return;
    auto& e = it->second;
    e.loaded = true;
    if (!d.ok || !device_ || d.bgra.empty()) {
        e.failed = true;
        return;
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width  = UINT(d.w);
    td.Height = UINT(d.h);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage     = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA srd{};
    srd.pSysMem      = d.bgra.data();
    srd.SysMemPitch  = UINT(d.w) * 4;

    HRESULT hr = device_->CreateTexture2D(&td, &srd, e.tex.put());
    if (FAILED(hr)) {
        e.failed = true;
        return;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
    svd.Format        = td.Format;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    hr = device_->CreateShaderResourceView(e.tex.get(), &svd, e.srv.put());
    if (FAILED(hr)) {
        e.failed = true;
        e.tex.reset();
        return;
    }
    e.width  = d.w;
    e.height = d.h;
}

void ThumbnailCache::worker_main() {
    for (;;) {
        std::wstring next;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] {
                return quit_.load() || !queue_.empty();
            });
            if (quit_.load()) return;
            next = std::move(queue_.front());
            queue_.pop_front();
        }

        std::vector<uint8_t> bgra;
        int w = 0, h = 0;
        bool ok = decode_one(next, kThumbW, kThumbH, bgra, w, h);
        Decoded d;
        d.path = std::move(next);
        d.bgra = std::move(bgra);
        d.w    = w;
        d.h    = h;
        d.ok   = ok;
        {
            std::lock_guard<std::mutex> lk(mu_);
            ready_.push_back(std::move(d));
        }
    }
}

bool ThumbnailCache::decode_one(const std::wstring& path,
                                int max_w, int max_h,
                                std::vector<uint8_t>& out_bgra,
                                int& out_w, int& out_h) {
    AVFormatContext* fmt = nullptr;
    std::string narrow = narrow_path(path);
    if (avformat_open_input(&fmt, narrow.c_str(), nullptr, nullptr) < 0) {
        log::warn("thumbnail: avformat_open_input failed for %s",
                  narrow.c_str());
        return false;
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    int vidx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vidx = int(i);
            break;
        }
    }
    if (vidx < 0) {
        avformat_close_input(&fmt);
        return false;
    }

    AVStream*       st     = fmt->streams[vidx];
    const AVCodec*  codec  = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
        avformat_close_input(&fmt);
        return false;
    }
    AVCodecContext* cc = avcodec_alloc_context3(codec);
    if (!cc) {
        avformat_close_input(&fmt);
        return false;
    }
    if (avcodec_parameters_to_context(cc, st->codecpar) < 0
        || avcodec_open2(cc, codec, nullptr) < 0) {
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  fr  = av_frame_alloc();
    if (!pkt || !fr) {
        if (pkt) av_packet_free(&pkt);
        if (fr)  av_frame_free(&fr);
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return false;
    }

    bool got_frame = false;
    int  guard = 0;
    while (!got_frame && guard++ < 200 && av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index != vidx) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(cc, pkt) >= 0) {
            while (avcodec_receive_frame(cc, fr) >= 0) {
                got_frame = true;
                break;
            }
        }
        av_packet_unref(pkt);
    }
    // Flush in case we had EOF on a still image.
    if (!got_frame) {
        avcodec_send_packet(cc, nullptr);
        if (avcodec_receive_frame(cc, fr) >= 0) got_frame = true;
    }

    if (!got_frame) {
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return false;
    }

    // Compute scaled output dimensions, preserving aspect ratio.
    int sw = fr->width;
    int sh = fr->height;
    if (sw <= 0 || sh <= 0) {
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return false;
    }
    double sx = double(max_w) / double(sw);
    double sy = double(max_h) / double(sh);
    double s  = std::min(sx, sy);
    if (s > 1.0) s = 1.0;
    int dw = std::max(2, int(double(sw) * s));
    int dh = std::max(2, int(double(sh) * s));
    // Round to even for sane chroma stride.
    dw &= ~1;
    dh &= ~1;

    SwsContext* sws = sws_getContext(
        sw, sh, AVPixelFormat(fr->format),
        dw, dh, AV_PIX_FMT_BGRA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws) {
        av_frame_free(&fr);
        av_packet_free(&pkt);
        avcodec_free_context(&cc);
        avformat_close_input(&fmt);
        return false;
    }

    out_bgra.assign(size_t(dw) * size_t(dh) * 4u, 0u);
    uint8_t* dstp[1] = { out_bgra.data() };
    int      dstl[1] = { dw * 4 };
    sws_scale(sws, fr->data, fr->linesize, 0, sh, dstp, dstl);
    sws_freeContext(sws);

    out_w = dw;
    out_h = dh;

    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return true;
}

}  // namespace volchay::ui
