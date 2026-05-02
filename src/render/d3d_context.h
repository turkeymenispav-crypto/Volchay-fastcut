// Owns the D3D11 device, the swap-chain bound to the main window, and the
// render target view. Designed so the device can be created lazily on the
// first frame to keep cold start time low.
#pragma once

#include "platform/windows.h"
#include "util/com_ptr.h"

namespace volchay::render {

class D3DContext {
public:
    D3DContext();
    ~D3DContext();

    D3DContext(const D3DContext&) = delete;
    D3DContext& operator=(const D3DContext&) = delete;

    // Create device + swap chain bound to hwnd. Idempotent.
    bool initialize(HWND hwnd);
    bool initialized() const { return swap_chain_.get() != nullptr; }

    // Resize swap chain back buffer (call from WM_SIZE).
    void resize(UINT width, UINT height);

    // Begin the frame: clear the back buffer, set RTV.
    void begin_frame(const float clear_rgba[4]);

    // End the frame: present.
    void end_frame(bool vsync);

    ID3D11Device*        device()  const { return device_.get(); }
    ID3D11DeviceContext* context() const { return context_.get(); }
    IDXGISwapChain1*     swap_chain() const { return swap_chain_.get(); }

private:
    void release_render_target();
    void create_render_target();

    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGISwapChain1>        swap_chain_;
    ComPtr<ID3D11RenderTargetView> rtv_;

    UINT  width_  = 0;
    UINT  height_ = 0;
    HWND  hwnd_   = nullptr;
};

}  // namespace volchay::render
