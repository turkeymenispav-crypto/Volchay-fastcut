#include "render/d3d_context.h"

#include "util/log.h"

namespace volchay::render {

D3DContext::D3DContext()  = default;
D3DContext::~D3DContext() = default;

bool D3DContext::initialize(HWND hwnd) {
    if (initialized()) return true;
    hwnd_ = hwnd;

    // Try hardware first, then WARP. Note: BGRA flag lets us interop with
    // Direct2D in the future.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL got_level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        device_.put(), &got_level, context_.put());

    if (FAILED(hr)) {
        log::warn("D3D11CreateDevice(HW) failed 0x%08lx, falling back to WARP",
                  long(hr));
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, ARRAYSIZE(levels),
            D3D11_SDK_VERSION,
            device_.put(), &got_level, context_.put());
        if (FAILED(hr)) {
            log::err("D3D11CreateDevice(WARP) failed 0x%08lx", long(hr));
            return false;
        }
    }

    // Get a DXGI factory from the device.
    ComPtr<IDXGIDevice>  dxgi_dev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(device_->QueryInterface(__uuidof(IDXGIDevice), dxgi_dev.put_void())))
        return false;
    if (FAILED(dxgi_dev->GetAdapter(adapter.put())))
        return false;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void())))
        return false;

    RECT rc{};
    ::GetClientRect(hwnd_, &rc);
    width_  = (UINT)(rc.right  - rc.left);
    height_ = (UINT)(rc.bottom - rc.top);
    if (width_  == 0) width_  = 1;
    if (height_ == 0) height_ = 1;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width              = width_;
    desc.Height             = height_;
    desc.Format             = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count   = 1;
    desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount        = 2;
    desc.Scaling            = DXGI_SCALING_NONE;
    desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(device_.get(), hwnd_, &desc,
                                         nullptr, nullptr, swap_chain_.put());
    if (FAILED(hr)) {
        log::err("CreateSwapChainForHwnd failed 0x%08lx", long(hr));
        return false;
    }

    // We handle Alt+Enter ourselves (or not at all).
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

    create_render_target();
    log::info("D3D11 initialised; feature level 0x%x, %ux%u",
              (unsigned)got_level, width_, height_);
    return true;
}

void D3DContext::release_render_target() { rtv_.reset(); }

void D3DContext::create_render_target() {
    if (!swap_chain_) return;
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D), back.put_void())))
        return;
    device_->CreateRenderTargetView(back.get(), nullptr, rtv_.put());
}

void D3DContext::resize(UINT width, UINT height) {
    if (!swap_chain_) return;
    if (width == 0 || height == 0) return;
    width_  = width;
    height_ = height;
    release_render_target();
    HRESULT hr = swap_chain_->ResizeBuffers(0, width_, height_,
                                            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        log::err("ResizeBuffers failed 0x%08lx", long(hr));
        return;
    }
    create_render_target();
}

void D3DContext::begin_frame(const float clear_rgba[4]) {
    if (!rtv_) create_render_target();
    if (!rtv_) return;
    ID3D11RenderTargetView* rtv = rtv_.get();
    context_->OMSetRenderTargets(1, &rtv, nullptr);
    context_->ClearRenderTargetView(rtv, clear_rgba);
    D3D11_VIEWPORT vp{};
    vp.Width    = float(width_);
    vp.Height   = float(height_);
    vp.MinDepth = 0.f;
    vp.MaxDepth = 1.f;
    context_->RSSetViewports(1, &vp);
}

void D3DContext::end_frame(bool vsync) {
    if (!swap_chain_) return;
    swap_chain_->Present(vsync ? 1 : 0, 0);
}

}  // namespace volchay::render
