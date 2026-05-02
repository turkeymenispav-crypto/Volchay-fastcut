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
    ::MFCreateAttributes(src_attrs.put(), 4);
    src_attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
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
    src->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    // Force NV12 video (most encoders accept it natively).
    {
        ComPtr<IMFMediaType> wanted;
        ::MFCreateMediaType(wanted.put());
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        wanted->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_NV12);
        src->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                 nullptr, wanted.get());
    }
    {
        ComPtr<IMFMediaType> wanted;
        ::MFCreateMediaType(wanted.put());
        wanted->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        wanted->SetGUID(MF_MT_SUBTYPE,    MFAudioFormat_PCM);
        wanted->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        wanted->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,        2);
        wanted->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE,     16);
        wanted->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,     4);
        wanted->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
        src->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                 nullptr, wanted.get());
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

    // Output video type.
    ComPtr<IMFMediaType> out_video;
    ::MFCreateMediaType(out_video.put());
    out_video->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out_video->SetGUID(MF_MT_SUBTYPE,    encoder_subtype(p.prefer_hevc));
    out_video->SetUINT32(MF_MT_AVG_BITRATE, UINT32(p.video_bitrate));
    out_video->SetUINT32(MF_MT_INTERLACE_MODE,
                         MFVideoInterlace_Progressive);
    ::MFSetAttributeSize (out_video.get(), MF_MT_FRAME_SIZE,
                          UINT32(p.width), UINT32(p.height));
    ::MFSetAttributeRatio(out_video.get(), MF_MT_FRAME_RATE,
                          UINT32(p.fps_num), UINT32(p.fps_den));
    ::MFSetAttributeRatio(out_video.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    DWORD video_idx = 0;
    hr = sw->AddStream(out_video.get(), &video_idx);
    if (FAILED(hr)) {
        set_status("AddStream(video) failed");
        ::MFShutdown();
        return;
    }

    // Input video type (must match what the source reader delivers).
    ComPtr<IMFMediaType> in_video;
    src->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, in_video.put());
    sw->SetInputMediaType(video_idx, in_video.get(), nullptr);

    // Audio.
    DWORD audio_idx = (DWORD)-1;
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
        src->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                 in_audio.put());
        if (FAILED(sw->SetInputMediaType(audio_idx, in_audio.get(), nullptr))) {
            audio_idx = (DWORD)-1;
        }
    }

    if (FAILED(sw->BeginWriting())) {
        set_status("BeginWriting failed");
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
                    sw->WriteSample(video_idx, sample.get());
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
    } else {
        sw->Finalize();
        progress_.store(1.0f);
        set_status("Done");
        success_.store(true);
    }
    ::MFShutdown();
}

}  // namespace volchay::media
