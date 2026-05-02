// Media Foundation video reader. Decodes a single video stream from a file
// into a D3D11 texture that can be sampled by ImGui::Image. This is the
// V0 implementation: BGRA32 conversion is done by MF in software, frames
// are uploaded into a dynamic texture every time the playhead advances
// past the next decoded sample. NV12 -> RGB shader conversion (zero-copy
// GPU path) is planned for V1.
#pragma once

#include "core/clip.h"
#include "platform/windows.h"
#include "util/com_ptr.h"

namespace volchay::media {

class MfPlayer {
public:
    MfPlayer();
    ~MfPlayer();

    MfPlayer(const MfPlayer&) = delete;
    MfPlayer& operator=(const MfPlayer&) = delete;

    // One-time MFStartup; safe to call multiple times.
    static bool ensure_started();

    // Open a file. Returns false on failure (error logged).
    bool open(const std::wstring& path, ID3D11Device* device);

    // Tear down everything except MF runtime.
    void close();

    bool is_open() const { return reader_.get() != nullptr; }

    // Decoded frame metadata.
    int width()  const { return width_;  }
    int height() const { return height_; }
    double fps()    const { return fps_; }
    core::TimeUs duration() const { return duration_; }

    // Path of the currently-open file (UTF-16). Empty if none.
    const std::wstring& source_path() const { return source_path_; }

    // The PTS of the most recently decoded frame (or -1 if none).
    core::TimeUs current_pts() const { return current_pts_; }

    // True if the most recent open() configured a hardware (DXVA) reader.
    bool hardware_decode() const { return hardware_decode_; }

    // Shader resource view of the current frame texture (BGRA8). Stable
    // pointer between frames (we update the underlying texture in place).
    // Returns nullptr if no frame has been decoded yet.
    ID3D11ShaderResourceView* current_srv() const { return srv_.get(); }

    // Start an asynchronous seek to t (microseconds). The next call to
    // pump() will deliver the requested frame.
    void seek(core::TimeUs t);

    // Advance to the frame whose PTS is closest to (but not after) the
    // playhead. Returns true if a new frame was decoded into the texture.
    // dt_seconds is wall-clock since last call; only used in playback mode.
    bool pump(core::TimeUs playhead_us);

    // Whether the next open() should request DXVA hardware decode. Updated
    // from Settings before each open.
    void set_prefer_hardware(bool on) { prefer_hardware_ = on; }

private:
    bool create_reader(const std::wstring& path);
    bool configure_output_format();
    bool ensure_texture(int w, int h, ID3D11Device* device);
    bool decode_to(core::TimeUs target_us);

    ComPtr<IMFSourceReader>           reader_;
    ComPtr<IMFDXGIDeviceManager>      dxgi_manager_;
    ComPtr<ID3D11Device>              device_;
    ComPtr<ID3D11DeviceContext>       context_;
    ComPtr<ID3D11Texture2D>           texture_;     // dynamic, mapped per frame
    ComPtr<ID3D11ShaderResourceView>  srv_;
    std::wstring                      source_path_;

    int          width_       = 0;
    int          height_      = 0;
    LONG         stride_      = 0;     // bytes/row in MF buffer
    double       fps_         = 0.0;
    core::TimeUs duration_    = 0;
    core::TimeUs current_pts_ = -1;
    core::TimeUs pending_seek_ = -1;   // -1 = no pending seek
    bool         prefer_hardware_ = true;
    bool         hardware_decode_ = false;
};

}  // namespace volchay::media
