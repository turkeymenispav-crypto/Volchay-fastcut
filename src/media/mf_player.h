// Media Foundation video reader. Decodes a single video stream into a
// D3D11 dynamic texture that ImGui can sample.
//
// Threading model (V1):
//   - All MF work — MFCreateSourceReaderFromURL, ReadSample,
//     ConvertToContiguousBuffer, sample Lock — runs on a dedicated worker
//     thread that owns the COM apartment. The decoded RGBA bytes are
//     dropped into a system-memory back-buffer.
//   - The UI thread's pump() Map+memcpy+Unmap of the dynamic D3D11
//     texture is the only thing it does for video. ID3D11DeviceContext
//     map/unmap is cheap (a few ms on HD, 10-15 ms on 4K) and avoids the
//     hundreds-of-milliseconds hitches we used to take on open() + first
//     decode when everything ran on the UI thread.
#pragma once

#include "core/clip.h"
#include "platform/windows.h"
#include "util/com_ptr.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace volchay::media {

class MfPlayer {
public:
    MfPlayer();
    ~MfPlayer();

    MfPlayer(const MfPlayer&) = delete;
    MfPlayer& operator=(const MfPlayer&) = delete;

    // One-time MFStartup; safe to call multiple times.
    static bool ensure_started();

    // Asynchronous-friendly open. The worker accepts the request and the
    // UI thread does NOT block on decoder initialisation. Returns true if
    // the worker was successfully signalled. is_open() flips to true once
    // the source reader has been built and the metadata + texture are
    // ready (a few tens of ms later for typical files); width()/height()/
    // duration()/fps() flip from 0 at the same moment.
    bool open(const std::wstring& path, ID3D11Device* device);

    // Tear down everything, joining the worker thread.
    void close();

    bool is_open() const { return reader_open_.load(); }

    // Decoded frame metadata. All return 0 / false until the worker has
    // finished probing the source — keep polling.
    int    width()           const { return width_.load();    }
    int    height()          const { return height_.load();   }
    double fps()             const { return fps_.load();      }
    core::TimeUs duration()  const { return duration_.load(); }

    const std::wstring& source_path() const { return source_path_; }

    core::TimeUs current_pts() const { return current_pts_.load(); }

    bool hardware_decode() const { return hardware_decode_.load(); }

    // Stable across the lifetime of the open file (worker creates it
    // before flipping is_open()=true). Returns nullptr until the texture
    // exists.
    ID3D11ShaderResourceView* current_srv() const { return srv_.get(); }

    // Asynchronous seek. The worker will decode a fresh frame at t and
    // make it available via pump() on the UI thread.
    void seek(core::TimeUs t);

    // UI-thread side of the player. Uploads the latest decoded frame to
    // the dynamic texture (if a fresh one is waiting) and asks the worker
    // for another frame if the playhead has moved past the most recent
    // one. Returns true when a new frame was uploaded this call.
    bool pump(core::TimeUs playhead_us);

    void set_prefer_hardware(bool on) { prefer_hardware_.store(on); }

private:
    // ---- Worker thread (owns IMFSourceReader) ----
    void worker_main();
    bool worker_open(const std::wstring& path);
    void worker_release_reader();
    bool worker_create_reader(const std::wstring& path);
    bool worker_configure_output_format();
    bool worker_decode_one(core::TimeUs target_us);

    std::thread             worker_;
    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::atomic<bool>       quit_{false};

    // Inbox protected by mu_.
    bool         open_request_pending_  = false;
    bool         close_request_pending_ = false;
    std::wstring pending_path_;

    // Cross-thread signals.
    std::atomic<core::TimeUs> seek_target_{-1};
    std::atomic<core::TimeUs> target_pts_ {-1};
    std::atomic<bool>         decode_kick_{false};

    // Completion signal for open(): worker flips this true after
    // worker_open succeeds OR fails, so the UI thread can stop polling.
    // Reset to false at the start of every open() call.
    std::atomic<bool>         open_done_{false};

    // Reader-side state (worker only).
    ComPtr<IMFSourceReader>           reader_;
    ComPtr<IMFDXGIDeviceManager>      dxgi_manager_;
    LONG                              stride_ = 0;
    core::TimeUs                      pending_seek_ = -1;
    core::TimeUs                      worker_pts_   = -1;
    // True if the source reader output is NV12 and we have to convert it
    // to BGRA on the worker thread (fallback for AV1 / decoder paths
    // where MF refuses to give us RGB32 even with advanced processing).
    bool                              convert_nv12_ = false;

    // D3D11 (device + texture are thread-safe to create; ImmediateContext
    // is UI-thread only). Texture and SRV are created on the worker after
    // probing the source's frame size, then stay live until close().
    ComPtr<ID3D11Device>              device_;
    ComPtr<ID3D11DeviceContext>       context_;     // UI thread only.
    ComPtr<ID3D11Texture2D>           texture_;
    ComPtr<ID3D11ShaderResourceView>  srv_;

    // Atomic snapshot of probed metadata (worker writes, UI reads).
    std::atomic<bool>           reader_open_{false};
    std::atomic<int>            width_{0};
    std::atomic<int>            height_{0};
    std::atomic<double>         fps_{0.0};
    std::atomic<core::TimeUs>   duration_{0};
    std::atomic<core::TimeUs>   current_pts_{-1};
    std::atomic<bool>           prefer_hardware_{true};
    std::atomic<bool>           hardware_decode_{false};
    std::wstring                source_path_;

    // Frame hand-off. Worker fills shared_buf_ with BGRA32 rows
    // (rowstride = w*4), then sets frame_ready_=true. UI's pump() locks
    // buf_mu_, maps the dynamic texture, memcpys the rows, unmaps. Single
    // buffer is fine because the UI consumes every frame the worker
    // produces (the worker only produces a new one after the previous
    // was picked up — see decode_kick_).
    mutable std::mutex          buf_mu_;
    std::vector<unsigned char>  shared_buf_;
    int                         shared_buf_w_ = 0;
    int                         shared_buf_h_ = 0;
    core::TimeUs                shared_buf_pts_ = -1;
    std::atomic<bool>           frame_ready_{false};
};

}  // namespace volchay::media
