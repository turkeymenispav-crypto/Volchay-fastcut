// Asynchronous thumbnail decoder for the Library panel.
//
// One worker thread drains a queue of (path -> wanted_size) decode
// requests, opens each file with libavformat / libavcodec (so videos
// AND still images go through the same path), scales the first frame
// down to a small BGRA bitmap, and hands the bytes back to the UI
// thread which uploads them into an ID3D11Texture2D + SRV.
//
// The UI thread NEVER touches libav. The worker NEVER touches D3D11.
// All cross-thread state is guarded by a single mutex.
#pragma once

#include "platform/windows.h"
#include "util/com_ptr.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <d3d11.h>

namespace volchay::ui {

class ThumbnailCache {
public:
    ThumbnailCache();
    ~ThumbnailCache();

    ThumbnailCache(const ThumbnailCache&)            = delete;
    ThumbnailCache& operator=(const ThumbnailCache&) = delete;

    // Bind the D3D11 device that we'll create textures on. Must be
    // called once before get() returns anything useful.
    void set_device(ID3D11Device* device);

    struct Entry {
        ComPtr<ID3D11Texture2D>          tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        int width   = 0;
        int height  = 0;
        bool loaded = false;
        bool failed = false;
    };

    // Returns the entry for `path`. The first call submits a decode
    // request to the worker thread; subsequent calls return the cached
    // result. Safe to call every frame.
    const Entry* get(const std::wstring& path);

    // Visit every entry in deterministic insertion order (used by debug
    // overlays — not used by the library panel itself).
    void for_each(const std::function<void(const std::wstring&,
                                           const Entry&)>& fn) const;

private:
    void worker_main();
    bool decode_one(const std::wstring& path,
                    int max_w, int max_h,
                    std::vector<uint8_t>& out_bgra,
                    int& out_w, int& out_h);

    // Worker -> UI handoff structure.
    struct Decoded {
        std::wstring         path;
        std::vector<uint8_t> bgra;
        int                  w = 0;
        int                  h = 0;
        bool                 ok = false;
    };

    void upload_decoded(const Decoded& d);

    ID3D11Device*           device_ = nullptr;

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::atomic<bool>       quit_{false};

    std::unordered_map<std::wstring, Entry>          entries_;
    std::vector<std::wstring>                        order_;
    std::deque<std::wstring>                         queue_;
    std::deque<Decoded>                              ready_;

    std::thread             worker_;
};

}  // namespace volchay::ui
