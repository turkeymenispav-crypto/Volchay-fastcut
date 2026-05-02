#include "media/mf_player.h"

#include "util/log.h"

#include <atomic>
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

MfPlayer::MfPlayer()  = default;
MfPlayer::~MfPlayer() { close(); }

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

bool MfPlayer::create_reader(const std::wstring& path) {
    // First attempt: hardware decode through DXVA. Falls back to software
    // automatically if the source is DRM-protected, the driver doesn't
    // support DXVA, or the requested format conversion fails.
    auto try_open = [&](bool with_dxva) -> bool {
        ComPtr<IMFAttributes> attrs;
        if (FAILED(::MFCreateAttributes(attrs.put(), 6))) return false;

        // ENABLE_VIDEO_PROCESSING gives us colour-space conversion (so we
        // can request RGB32 even when the decoder produces NV12).
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

        HRESULT hr = ::MFCreateSourceReaderFromURL(path.c_str(), attrs.get(),
                                                   reader_.put());
        if (FAILED(hr)) {
            log::warn("MFCreateSourceReaderFromURL(%s) 0x%08lx (%s)",
                      with_dxva ? "HW" : "SW", long(hr), hr_name(hr));
            reader_.reset();
            dxgi_manager_.reset();
            return false;
        }
        reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        hardware_decode_ = with_dxva;
        return true;
    };

    hardware_decode_ = false;
    if (prefer_hardware_ && device_ && try_open(/*with_dxva=*/true)) {
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

bool MfPlayer::configure_output_format() {
    if (!reader_) return false;

    // Ask the reader to deliver RGB32 (BGRA on the wire). When the
    // ENABLE_VIDEO_PROCESSING attribute is set the reader inserts the
    // Color Converter MFT automatically, so this should succeed for any
    // stream the decoder can produce.
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
        // Some hardware decoders won't expose RGB32 conversion. Retry
        // with software decode to guarantee we get pixels we can upload.
        log::warn("RGB32 output unavailable on current reader; "
                  "rebuilding reader in software mode");
        reader_.reset();
        dxgi_manager_.reset();
        hardware_decode_ = false;
        if (!create_reader(source_path_)) return false;
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
    width_  = int(w);
    height_ = int(h);

    UINT32 num = 0, den = 0;
    if (SUCCEEDED(::MFGetAttributeRatio(got.get(), MF_MT_FRAME_RATE, &num, &den))
        && den != 0) {
        fps_ = double(num) / double(den);
    }

    LONG stride = 0;
    if (SUCCEEDED(got->GetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32*)&stride))) {
        stride_ = stride;
    } else {
        stride_ = LONG(width_) * 4;
    }

    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    if (SUCCEEDED(reader_->GetPresentationAttribute(
            MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))) {
        if (pv.vt == VT_UI8) {
            duration_ = mftime_to_us((LONGLONG)pv.uhVal.QuadPart);
        }
    }
    ::PropVariantClear(&pv);

    log::info("MF stream: %dx%d @ %.3f fps, stride %ld, duration %.3fs (%s)",
              width_, height_, fps_, long(stride_),
              core::to_seconds(duration_),
              hardware_decode_ ? "HW decode" : "SW decode");
    return width_ > 0 && height_ > 0;
}

bool MfPlayer::ensure_texture(int w, int h, ID3D11Device* device) {
    if (texture_ && srv_) return true;

    // Dynamic texture mapped with WRITE_DISCARD every frame. The
    // discard semantics tell the driver "I don't care about the old
    // contents, give me a fresh backing buffer if necessary" so we
    // never stall on the GPU read pipeline ImGui issues.
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

    HRESULT hr = device->CreateTexture2D(&td, nullptr, texture_.put());
    if (FAILED(hr)) {
        log::err("CreateTexture2D(dynamic) failed 0x%08lx", long(hr));
        return false;
    }
    hr = device->CreateShaderResourceView(texture_.get(), nullptr, srv_.put());
    if (FAILED(hr)) {
        log::err("CreateShaderResourceView 0x%08lx", long(hr));
        return false;
    }

    log::info("Video texture ready: %dx%d BGRA8 (dynamic)", w, h);
    return true;
}

bool MfPlayer::open(const std::wstring& path, ID3D11Device* device) {
    close();
    if (!ensure_started()) return false;
    if (!device)           return false;

    source_path_ = path;
    device_ = ComPtr<ID3D11Device>(device);   // AddRef'd by ComPtr ctor
    device_->GetImmediateContext(context_.put());

    if (!create_reader(path))         { close(); return false; }
    if (!configure_output_format())   { close(); return false; }
    if (!ensure_texture(width_, height_, device_.get())) {
        close(); return false;
    }

    pending_seek_ = 0;
    current_pts_  = -1;
    return true;
}

void MfPlayer::close() {
    reader_.reset();
    dxgi_manager_.reset();
    srv_.reset();
    texture_.reset();
    context_.reset();
    device_.reset();
    source_path_.clear();
    width_ = height_ = 0;
    stride_ = 0;
    fps_ = 0.0;
    duration_ = 0;
    current_pts_  = -1;
    pending_seek_ = -1;
    hardware_decode_ = false;
}

void MfPlayer::seek(core::TimeUs t) {
    if (!reader_) return;
    if (t < 0) t = 0;
    if (duration_ > 0 && t > duration_) t = duration_;
    pending_seek_ = t;
}

bool MfPlayer::decode_to(core::TimeUs target_us) {
    if (!reader_) return false;

    if (pending_seek_ >= 0) {
        PROPVARIANT pv;
        ::PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = us_to_mftime(pending_seek_);
        reader_->SetCurrentPosition(GUID_NULL, pv);
        ::PropVariantClear(&pv);
        pending_seek_ = -1;
        current_pts_  = -1;
    }

    int sample_count = 0;
    while (true) {
        if (++sample_count > 64) {
            log::warn("decode_to: 64 samples without delivering one — bailing");
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
            configure_output_format();
        }
        if (!sample) {
            // Stream tick without a sample. Loop again.
            continue;
        }

        core::TimeUs pts = mftime_to_us(timestamp_mf);

        bool keep = true;
        if (fps_ > 0.0 && pts + core::TimeUs(1'000'000.0 / fps_) < target_us) {
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

        // Map the dynamic texture with DISCARD and copy each row,
        // honouring the (possibly negative) MF stride so bottom-up
        // RGB32 streams render right-side up.
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr_map = context_->Map(texture_.get(), 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr_map)) {
            const LONG src_stride = stride_ != 0 ? stride_ : LONG(width_) * 4;
            const BYTE* src = data;
            BYTE*       dst = (BYTE*)mapped.pData;
            const bool flip = src_stride < 0;
            const LONG row  = flip ? -src_stride : src_stride;
            if (flip) src = data + LONG(height_ - 1) * row;
            const size_t row_bytes = size_t(width_) * 4;
            for (int y = 0; y < height_; ++y) {
                std::memcpy(dst + size_t(y) * mapped.RowPitch, src, row_bytes);
                src = flip ? src - row : src + row;
            }
            context_->Unmap(texture_.get(), 0);
        } else {
            log::warn("Map(dynamic) failed 0x%08lx", long(hr_map));
        }

        buffer->Unlock();
        if (current_pts_ < 0) {
            log::info("First frame uploaded (pts=%.3fs, %dx%d, %s)",
                      core::to_seconds(pts), width_, height_,
                      hardware_decode_ ? "HW" : "SW");
        }
        current_pts_ = pts;
        return true;
    }
}

bool MfPlayer::pump(core::TimeUs playhead_us) {
    if (!reader_) return false;

    if (current_pts_ < 0) {
        return decode_to(playhead_us < 0 ? 0 : playhead_us);
    }

    if (pending_seek_ >= 0) {
        return decode_to(pending_seek_);
    }

    if (fps_ > 0.0) {
        const core::TimeUs frame_time = core::TimeUs(1'000'000.0 / fps_);
        if (playhead_us >= current_pts_ + frame_time) {
            return decode_to(playhead_us);
        }
    } else if (playhead_us > current_pts_ + 16'000) {
        return decode_to(playhead_us);
    }
    return false;
}

}  // namespace volchay::media
