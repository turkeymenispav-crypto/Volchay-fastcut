#include "media/exporter.h"

#include "media/mf_extras.h"
#include "util/com_ptr.h"
#include "util/log.h"

#include <chrono>
#include <cstring>

namespace volchay::media {

using volchay::ComPtr;

const ExportPreset kPresets[] = {
    {"H.264 1080p / 12 Mbps",         1920, 1080, 12'000'000, 60, 1, false},
    {"H.264 1080p / 8 Mbps  (web)",   1920, 1080,  8'000'000, 60, 1, false},
    {"H.264 4K   / 35 Mbps",          3840, 2160, 35'000'000, 60, 1, false},
    {"HEVC 4K HDR / 50 Mbps",         3840, 2160, 50'000'000, 60, 1, true},
    {"H.264 720p / 5 Mbps  (mobile)", 1280,  720,  5'000'000, 30, 1, false},
};
const int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

namespace {
LONGLONG us_to_mftime(core::TimeUs us) { return LONGLONG(us) * 10; }

// Map the user-supplied bitrate at the chosen preset's resolution onto a
// reasonable BlockAlignment / GOP for H.264.
GUID encoder_subtype(bool hevc) {
    return hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
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

void Exporter::worker_main() {
    HRESULT init = ::CoInitializeEx(nullptr,
                                    COINIT_MULTITHREADED | COINIT_DISABLE_OLE1DDE);
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
            busy_.store(false);
            finished_.store(true);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (SUCCEEDED(init)) ::CoUninitialize();
}

void Exporter::run_one_export(ExportRequest req) {
    auto set_status = [&](std::string s) {
        std::lock_guard lk(mu_);
        status_ = std::move(s);
    };
    set_status("Preparing source...");

    if (req.preset_index < 0 || req.preset_index >= kPresetCount) {
        set_status("Invalid preset");
        success_.store(false);
        return;
    }
    const ExportPreset& p = kPresets[req.preset_index];

    HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) {
        set_status("MFStartup failed");
        return;
    }

    // Source reader on the input. With ENABLE_HARDWARE_TRANSFORMS set,
    // MF picks the GPU-resident H.264/HEVC decoder MFT when one is
    // available — on a 4060 that means NVDEC for input.
    ComPtr<IMFAttributes> src_attrs;
    ::MFCreateAttributes(src_attrs.put(), 5);
    // Enable both basic and advanced video processing. The advanced flag
    // is what makes Media Foundation insert a Color Converter MFT for
    // AV1 / 10-bit HEVC sources whose decoder output is P010 — without
    // it the source reader silently keeps the decoder's native type and
    // the H.264/HEVC encoder rejects every WriteSample with E_INVALIDARG.
    src_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    // MinGW's mfreadwrite.h doesn't expose this attribute even though it's
    // been in the Windows SDK since Win8. The GUID is stable.
    static const GUID kMfReaderAdvancedVideoProcessing = {
        0x0f81da2c, 0xb537, 0x4672,
        { 0xa8, 0xb2, 0xa6, 0x81, 0xb1, 0x73, 0x07, 0xa3 } };
    src_attrs->SetUINT32(kMfReaderAdvancedVideoProcessing, TRUE);
    src_attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                         req.hardware ? TRUE : FALSE);
    src_attrs->SetUINT32(MF_SOURCE_READER_DISABLE_DXVA,
                         req.hardware ? FALSE : TRUE);

    ComPtr<IMFSourceReader> src;
    hr = ::MFCreateSourceReaderFromURL(req.source_path.c_str(), src_attrs.get(),
                                       src.put());
    if (FAILED(hr)) {
        set_status("Cannot open source");
        ::MFShutdown();
        return;
    }
    src->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    src->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    bool source_has_audio =
        SUCCEEDED(src->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                          TRUE));

    // Probe the source's native video size + frame rate so we can set
    // matching attributes on both the source-reader output type and the
    // sink-writer output type. Without explicit size/fps the source
    // reader silently keeps the decoder's default media type (or the
    // partial NV12 type fails to negotiate), and the sink writer's H.264
    // encoder rejects every frame — producing an audio-only .mp4.
    UINT32 src_w = 0, src_h = 0, fps_num = 30, fps_den = 1;
    {
        ComPtr<IMFMediaType> native;
        if (SUCCEEDED(src->GetNativeMediaType(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, native.put()))) {
            ::MFGetAttributeSize (native.get(), MF_MT_FRAME_SIZE, &src_w, &src_h);
            ::MFGetAttributeRatio(native.get(), MF_MT_FRAME_RATE,
                                  &fps_num, &fps_den);
        }
    }
    if (src_w == 0 || src_h == 0) {
        set_status("Source has no readable video stream");
        ::MFShutdown();
        return;
    }
    if (fps_num == 0) { fps_num = 30; fps_den = 1; }

    // Force NV12 video at the source's native size + rate. The encoder
    // negotiation will then succeed because the input type carries every
    // attribute (subtype, frame size, frame rate, interlace) the H.264
    // encoder needs.
    {
        ComPtr<IMFMediaType> wanted;
        ::MFCreateMediaType(wanted.put());
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        wanted->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_NV12);
        wanted->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        ::MFSetAttributeSize (wanted.get(), MF_MT_FRAME_SIZE,  src_w,   src_h);
        ::MFSetAttributeRatio(wanted.get(), MF_MT_FRAME_RATE,  fps_num, fps_den);
        ::MFSetAttributeRatio(wanted.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        hr = src->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                      nullptr, wanted.get());
        if (FAILED(hr)) {
            set_status("Cannot set NV12 on source video stream");
            ::MFShutdown();
            return;
        }
    }
    if (source_has_audio) {
        ComPtr<IMFMediaType> wanted;
        ::MFCreateMediaType(wanted.put());
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        wanted->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
        wanted->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        wanted->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        2);
        wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     16);
        wanted->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,     4);
        wanted->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        if (FAILED(src->SetCurrentMediaType(
                MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, wanted.get()))) {
            // No fatal: keep going without audio.
            source_has_audio = false;
        }
    }

    // Probe duration for progress reporting.
    core::TimeUs total_us = 0;
    {
        PROPVARIANT pv;
        ::PropVariantInit(&pv);
        if (SUCCEEDED(src->GetPresentationAttribute(
                MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv))
            && pv.vt == VT_UI8) {
            total_us = core::TimeUs(pv.uhVal.QuadPart / 10);
        }
        ::PropVariantClear(&pv);
    }
    if (req.trim_end_us < 0 || req.trim_end_us > total_us) {
        req.trim_end_us = total_us;
    }
    const core::TimeUs span_us = std::max<core::TimeUs>(
        1, req.trim_end_us - req.trim_start_us);

    // Sink writer. The hardware-transform attribute is what actually
    // routes the output through NVENC / Quick Sync / AMF; without it
    // MF defaults to the Microsoft SW H.264 encoder which is ~30x
    // slower than NVENC on a 4060.
    ComPtr<IMFAttributes> sink_attrs;
    ::MFCreateAttributes(sink_attrs.put(), 6);
    sink_attrs->SetGUID  (MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    sink_attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                          req.hardware ? TRUE : FALSE);
    sink_attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    sink_attrs->SetUINT32(MF_LOW_LATENCY, FALSE);

    ComPtr<IMFSinkWriter> sw;
    hr = ::MFCreateSinkWriterFromURL(req.output_path.c_str(), nullptr,
                                     sink_attrs.get(), sw.put());
    if (FAILED(hr)) {
        set_status("Cannot create output");
        ::MFShutdown();
        return;
    }

    // Output video type. We keep the source's native frame size + rate
    // (preset.width/height/fps_* are intentionally ignored in V1) — that
    // way the H.264 encoder sees matching input + output dimensions and
    // doesn't need an inserted resizer MFT, which MF won't add
    // automatically when driving SinkWriter directly. The preset still
    // controls the bitrate and the H.264-vs-HEVC choice.
    ComPtr<IMFMediaType> out_video;
    ::MFCreateMediaType(out_video.put());
    out_video->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_video->SetGUID(MF_MT_SUBTYPE,    encoder_subtype(p.prefer_hevc));
    out_video->SetUINT32(MF_MT_AVG_BITRATE, UINT32(p.video_bitrate));
    out_video->SetUINT32(MF_MT_INTERLACE_MODE,
                         MFVideoInterlace_Progressive);
    ::MFSetAttributeSize (out_video.get(), MF_MT_FRAME_SIZE,  src_w,   src_h);
    ::MFSetAttributeRatio(out_video.get(), MF_MT_FRAME_RATE,  fps_num, fps_den);
    ::MFSetAttributeRatio(out_video.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    DWORD video_idx = 0;
    hr = sw->AddStream(out_video.get(), &video_idx);
    if (FAILED(hr)) {
        set_status("AddStream(video) failed (encoder unavailable?)");
        log::err("Exporter: AddStream(video) hr=0x%08lx", (long)hr);
        ::MFShutdown();
        return;
    }

    // Input video type. Build explicitly so we control MF_MT_DEFAULT_STRIDE
    // and PIXEL_ASPECT_RATIO — the encoder validates samples against these
    // and will reject WriteSample with E_INVALIDARG if they're missing or
    // mismatch the actual sample stride (typical AV1 path: the source
    // reader's NV12 output has a stride attribute we need to surface to
    // the encoder).
    ComPtr<IMFMediaType> reader_type;
    hr = src->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                  reader_type.put());
    if (FAILED(hr)) {
        set_status("Source video type unavailable");
        log::err("Exporter: GetCurrentMediaType(video) hr=0x%08lx", (long)hr);
        ::MFShutdown();
        return;
    }
    UINT32 reader_stride_u = UINT32(src_w);   // NV12 default stride = width
    reader_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &reader_stride_u);

    ComPtr<IMFMediaType> in_video;
    ::MFCreateMediaType(in_video.put());
    in_video->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in_video->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_NV12);
    in_video->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in_video->SetUINT32(MF_MT_DEFAULT_STRIDE, reader_stride_u);
    ::MFSetAttributeSize (in_video.get(), MF_MT_FRAME_SIZE,  src_w,   src_h);
    ::MFSetAttributeRatio(in_video.get(), MF_MT_FRAME_RATE,  fps_num, fps_den);
    ::MFSetAttributeRatio(in_video.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = sw->SetInputMediaType(video_idx, in_video.get(), nullptr);
    if (FAILED(hr)) {
        set_status("Encoder rejected source video format");
        log::err("Exporter: SetInputMediaType(video) hr=0x%08lx", (long)hr);
        ::MFShutdown();
        return;
    }
    log::info("Exporter: input video set to NV12 %ux%u stride=%u @ %u/%u fps",
              src_w, src_h, reader_stride_u, fps_num, fps_den);

    // Audio.
    DWORD audio_idx = (DWORD)-1;
    if (source_has_audio) {
        ComPtr<IMFMediaType> out_audio;
        ::MFCreateMediaType(out_audio.put());
        out_audio->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out_audio->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_AAC);
        out_audio->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        out_audio->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        2);
        out_audio->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     16);
        out_audio->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24'000);   // 192 kbps / 8
        out_audio->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);

        if (SUCCEEDED(sw->AddStream(out_audio.get(), &audio_idx))) {
            ComPtr<IMFMediaType> in_audio;
            if (SUCCEEDED(src->GetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM, in_audio.put()))
                && SUCCEEDED(sw->SetInputMediaType(audio_idx,
                                                   in_audio.get(), nullptr))) {
                // ok
            } else {
                audio_idx = (DWORD)-1;
            }
        }
    }

    hr = sw->BeginWriting();
    if (FAILED(hr)) {
        set_status("BeginWriting failed");
        log::err("Exporter: BeginWriting hr=0x%08lx", (long)hr);
        ::MFShutdown();
        return;
    }

    // Seek to trim start.
    if (req.trim_start_us > 0) {
        PROPVARIANT pv;
        ::PropVariantInit(&pv);
        pv.vt = VT_I8;
        pv.hVal.QuadPart = us_to_mftime(req.trim_start_us);
        src->SetCurrentPosition(GUID_NULL, pv);
        ::PropVariantClear(&pv);
    }

    set_status("Encoding...");
    bool video_done = false, audio_done = (audio_idx == (DWORD)-1);
    bool video_write_failed = false;
    HRESULT first_video_err = S_OK;
    UINT64 video_samples_written = 0;

    while (!cancel_.load() && (!video_done || !audio_done)) {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;

        if (!video_done) {
            hr = src->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0,
                                 &stream, &flags, &ts, sample.put());
            if (SUCCEEDED(hr) && sample) {
                core::TimeUs pts = core::TimeUs(ts / 10);
                if (pts > req.trim_end_us) {
                    video_done = true;
                } else {
                    sample->SetSampleTime(us_to_mftime(pts - req.trim_start_us));
                    HRESULT wh = sw->WriteSample(video_idx, sample.get());
                    if (FAILED(wh)) {
                        if (!video_write_failed) {
                            first_video_err = wh;
                            log::err("Exporter: WriteSample(video) hr=0x%08lx",
                                     (long)wh);
                        }
                        video_write_failed = true;
                    } else {
                        ++video_samples_written;
                    }
                    progress_.store(float(double(pts - req.trim_start_us) /
                                          double(span_us)));
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) video_done = true;
        }

        if (!audio_done) {
            sample.reset();
            hr = src->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0,
                                 &stream, &flags, &ts, sample.put());
            if (SUCCEEDED(hr) && sample) {
                core::TimeUs pts = core::TimeUs(ts / 10);
                if (pts > req.trim_end_us) {
                    audio_done = true;
                } else {
                    sample->SetSampleTime(us_to_mftime(pts - req.trim_start_us));
                    sw->WriteSample(audio_idx, sample.get());
                }
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) audio_done = true;
        }
    }

    if (cancel_.load()) {
        set_status("Cancelled");
        sw->Finalize();
        success_.store(false);
    } else if (video_samples_written == 0) {
        // No video frames made it past the encoder. Without this check
        // the .mp4 would be finalized with an audio-only payload and the
        // user would think "the export ran but my video is gone".
        sw->Finalize();
        char buf[96];
        ::snprintf(buf, sizeof(buf),
                   "Export failed: encoder rejected video (hr=0x%08lx)",
                   (long)first_video_err);
        set_status(buf);
        log::err("%s", buf);
        success_.store(false);
    } else {
        sw->Finalize();
        progress_.store(1.0f);
        set_status("Done");
        success_.store(true);
    }
    ::MFShutdown();
}

}  // namespace volchay::media
