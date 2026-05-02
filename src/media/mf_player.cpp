#include "media/mf_player.h"

#include "util/log.h"

#include <atomic>
#include <chrono>
#include <cstring>

namespace volchay::media {
namespace {

std::atomic<bool> g_mf_started{false};

constexpr LONGLONG us_to_mftime(core::TimeUs us) {
    return LONGLONG(us) * 10;
}

constexpr core::TimeUs mftime_to_us(LONGLONG t) {
    return core::TimeUs(t / 10);
}

const char* hr_name(HRESULT hr) {
    switch (hr) {
    case MF_E_INVALIDMEDIATYPE: return "MF_E_INVALIDMEDIATYPE";
    case MF_E_TOPO_CODEC_NOT_FOUND: return "MF_E_TOPO_CODEC_NOT_FOUND";
    case MF_E_UNSUPPORTED_BYTESTREAM_TYPE: return "MF_E_UNSUPPORTED_BYTESTREAM_TYPE";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_NOTIMPL:    return "E_NOTIMPL";
    case E_FAIL:       return "E_FAIL";
    default: return "?";
    }
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

bool MfPlayer::ensure_started() {
    bool expected = false;
    if (!g_mf_started.compare_exchange_strong(expected, true)) {
        return true;
    }
    HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        log::err("MFStartup failed 0x%08lx (%s)", long(hr), hr_name(hr));
        g_mf_started.store(false);
        return false;
    }
    log::info("Media Foundation started (MF_VERSION=0x%x)", (unsigned)MF_VERSION);
    return true;
}

// ---------------------------------------------------------------------------
// UI thread API
// ---------------------------------------------------------------------------

bool MfPlayer::open(const std::wstring& path, ID3D11Device* device) {
    if (!device) return false;

    // Tear down any previous open synchronously (so the worker is idle
    // before we hand it a new request).
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

    // Block on the metadata-probe phase only. The slow part — decoder
    // init for HEVC/4K — happens here on the worker, but the UI thread
    // call site needs duration/width/height available the moment
    // open() returns (project.add_media + set_single_clip read them
    // synchronously). The first frame decode that follows is what we
    // really want off the UI thread, and that runs asynchronously now.
    // Cap at 5 s so a hung decoder can't permanently freeze the UI.
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
    // Wait for the worker to actually release the reader.
    for (int i = 0; i < 1000 && reader_open_.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // UI-side state.
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
    // Pre-emptively reflect the seek on current_pts_ — without this the
    // viewer briefly shows a stale frame's PTS until the worker decodes
    // the new one.
    current_pts_.store(t);
    cv_.notify_all();
}

bool MfPlayer::pump(core::TimeUs playhead_us) {
    if (!reader_open_.load()) return false;

    // Tell the worker how far ahead we want frames to be ready. This is
    // also how the worker knows we're "playing forward" — when target_pts
    // moves past current_pts the worker decodes the next frame.
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

    // Decide whether to ask the worker for another frame.
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
    HRESULT init = ::CoInitializeEx(nullptr,
                                    COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);

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
            worker_release_reader();
        }
        if (do_open) {
            // close any still-live previous reader
            worker_release_reader();
            if (!worker_open(path)) {
                log::err("MfPlayer worker: open failed");
                worker_release_reader();
            }
            // Either path: signal completion to the UI thread.
            open_done_.store(true);
        }

        if (!reader_open_.load()) continue;

        // Apply pending seek before decode.
        const core::TimeUs seek = seek_target_.exchange(-1);
        if (seek >= 0 && reader_) {
            PROPVARIANT pv;
            ::PropVariantInit(&pv);
            pv.vt = VT_I8;
            pv.hVal.QuadPart = us_to_mftime(seek);
            reader_->SetCurrentPosition(GUID_NULL, pv);
            ::PropVariantClear(&pv);
            pending_seek_ = seek;
            worker_pts_   = -1;
        }

        // Decode at most one frame per signal — UI re-kicks us when it
        // wants the next one. This caps decode work to the actual
        // display rate without ever doing it on the UI thread.
        if (decode_kick_.exchange(false)) {
            const core::TimeUs target = target_pts_.load();
            worker_decode_one(target);
        }
    }

    worker_release_reader();
    if (SUCCEEDED(init)) ::CoUninitialize();
}

bool MfPlayer::worker_open(const std::wstring& path) {
    if (!ensure_started())  return false;
    if (!device_)           return false;

    if (!worker_create_reader(path))         return false;
    if (!worker_configure_output_format())   return false;

    // Create the dynamic texture. ID3D11Device::CreateTexture2D /
    // CreateShaderResourceView are thread-safe in default (non-singlethreaded)
    // device creation mode, which D3DContext uses.
    const int w = width_.load();
    const int h = height_.load();
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
    pending_seek_ = -1;
    worker_pts_   = -1;

    log::info("Video texture ready: %dx%d BGRA8 (dynamic)", w, h);
    reader_open_.store(true);
    return true;
}

void MfPlayer::worker_release_reader() {
    reader_open_.store(false);
    reader_.reset();
    dxgi_manager_.reset();
    width_.store(0);
    height_.store(0);
    fps_.store(0.0);
    duration_.store(0);
    hardware_decode_.store(false);
    pending_seek_ = -1;
    worker_pts_   = -1;
}

bool MfPlayer::worker_create_reader(const std::wstring& path) {
    auto try_open = [&](bool with_dxva) -> bool {
        ComPtr<IMFAttributes> attrs;
        if (FAILED(::MFCreateAttributes(attrs.put(), 6))) return false;

        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        attrs->SetUINT32(MF_LOW_LATENCY, FALSE);

        if (with_dxva && device_) {
            UINT reset_token = 0;
            ComPtr<IMFDXGIDeviceManager> mgr;
            HRESULT hr = ::MFCreateDXGIDeviceManager(&reset_token, mgr.put());
            if (SUCCEEDED(hr) && SUCCEEDED(mgr->ResetDevice(device_.get(),
                                                            reset_token))) {
                attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, mgr.get());
                attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, FALSE);
                dxgi_manager_ = mgr;
            } else {
                log::warn("MFCreateDXGIDeviceManager/ResetDevice 0x%08lx",
                          long(hr));
                return false;
            }
        } else {
            attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA, TRUE);
        }

        ComPtr<IMFSourceReader> r;
        HRESULT hr = ::MFCreateSourceReaderFromURL(path.c_str(), attrs.get(),
                                                   r.put());
        if (FAILED(hr)) {
            log::warn("MFCreateSourceReaderFromURL(%s) 0x%08lx (%s)",
                      with_dxva ? "HW" : "SW", long(hr), hr_name(hr));
            dxgi_manager_.reset();
            return false;
        }
        r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        r->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        reader_ = r;
        hardware_decode_.store(with_dxva);
        return true;
    };

    hardware_decode_.store(false);
    if (prefer_hardware_.load() && device_ && try_open(/*with_dxva=*/true)) {
        log::info("MF: source reader opened (DXVA hardware decode)");
        return true;
    }
    if (!try_open(/*with_dxva=*/false)) {
        log::err("MF: software open also failed");
        return false;
    }
    log::info("MF: source reader opened (software decode)");
    return true;
}

bool MfPlayer::worker_configure_output_format() {
    if (!reader_) return false;

    auto try_set = [&](const GUID& subtype) -> bool {
        ComPtr<IMFMediaType> out_type;
        if (FAILED(::MFCreateMediaType(out_type.put()))) return false;
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE,    subtype);
        HRESULT hr = reader_->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out_type.get());
        if (FAILED(hr)) {
            log::warn("SetCurrentMediaType 0x%08lx", long(hr));
            return false;
        }
        return true;
    };

    if (!try_set(MFVideoFormat_RGB32)) {
        log::warn("RGB32 output unavailable on current reader; "
                  "rebuilding reader in software mode");
        reader_.reset();
        dxgi_manager_.reset();
        hardware_decode_.store(false);
        // Reuse the same path stored on the UI side. This worker is the
        // only thread that touches reader_, so it's safe to call
        // worker_create_reader here.
        if (!worker_create_reader(source_path_)) return false;
        if (!try_set(MFVideoFormat_RGB32)) {
            log::err("RGB32 output unavailable even on SW reader");
            return false;
        }
    }

    ComPtr<IMFMediaType> got;
    if (FAILED(reader_->GetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, got.put()))) {
        return false;
    }

    UINT32 w = 0, h = 0;
    ::MFGetAttributeSize(got.get(), MF_MT_FRAME_SIZE, &w, &h);
    width_.store(int(w));
    height_.store(int(h));

    UINT32 num = 0, den = 0;
    if (SUCCEEDED(::MFGetAttributeRatio(got.get(), MF_MT_FRAME_RATE, &num, &den))
        && den != 0) {
        fps_.store(double(num) / double(den));
    }

    LONG stride = 0;
    if (SUCCEEDED(got->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride))) {
        stride_ = stride;
    } else {
        stride_ = LONG(int(w)) * 4;
    }

    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    if (SUCCEEDED(reader_->GetPresentationAttribute(
            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) {
            duration_.store(mftime_to_us((LONGLONG)pv.uhVal.QuadPart));
        }
    }
    ::PropVariantClear(&pv);

    log::info("MF stream: %dx%d @ %.3f fps, stride %ld, duration %.3fs (%s)",
              int(w), int(h), fps_.load(), long(stride_),
              core::to_seconds(duration_.load()),
              hardware_decode_.load() ? "HW decode" : "SW decode");
    return w > 0 && h > 0;
}

bool MfPlayer::worker_decode_one(core::TimeUs target_us) {
    if (!reader_) return false;

    int sample_count = 0;
    while (true) {
        if (++sample_count > 64) {
            log::warn("worker_decode_one: 64 reads without a sample — bailing");
            return false;
        }
        DWORD       stream_index = 0;
        DWORD       flags        = 0;
        LONGLONG    timestamp_mf = 0;
        ComPtr<IMFSample> sample;

        HRESULT hr = reader_->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
            &stream_index, &flags, &timestamp_mf, sample.put());
        if (FAILED(hr)) {
            log::err("ReadSample 0x%08lx (%s)", long(hr), hr_name(hr));
            return false;
        }
        if (flags & MF_SOURCE_READERF_ERROR) {
            log::err("ReadSample reported MF_SOURCE_READERF_ERROR");
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            log::info("ReadSample: end of stream");
            return false;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            log::info("ReadSample: media type changed mid-stream; reconfiguring");
            worker_configure_output_format();
        }
        if (!sample) continue;  // stream tick without payload

        const core::TimeUs pts = mftime_to_us(timestamp_mf);

        bool keep = true;
        const double f = fps_.load();
        if (f > 0.0 && pts + core::TimeUs(1'000'000.0 / f) < target_us) {
            keep = false;
        }
        if (!keep) continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) {
            continue;
        }
        BYTE* data = nullptr;
        DWORD max_len = 0, cur_len = 0;
        if (FAILED(buffer->Lock(&data, &max_len, &cur_len))) {
            continue;
        }

        // Copy into shared_buf_ honouring (possibly negative) MF stride.
        {
            std::lock_guard lk(buf_mu_);
            const int w = shared_buf_w_;
            const int h = shared_buf_h_;
            const LONG src_stride = stride_ != 0 ? stride_ : LONG(w) * 4;
            const BYTE* src = data;
            const bool flip = src_stride < 0;
            const LONG row  = flip ? -src_stride : src_stride;
            if (flip) src = data + LONG(h - 1) * row;
            const size_t row_bytes = size_t(w) * 4;
            unsigned char* dst = shared_buf_.data();
            for (int y = 0; y < h; ++y) {
                std::memcpy(dst + size_t(y) * row_bytes, src, row_bytes);
                src = flip ? src - row : src + row;
            }
            shared_buf_pts_ = pts;
        }

        buffer->Unlock();
        if (worker_pts_ < 0) {
            log::info("First frame decoded (pts=%.3fs, %dx%d, %s)",
                      core::to_seconds(pts), width_.load(), height_.load(),
                      hardware_decode_.load() ? "HW" : "SW");
        }
        worker_pts_ = pts;
        frame_ready_.store(true);
        return true;
    }
}

}  // namespace volchay::media
