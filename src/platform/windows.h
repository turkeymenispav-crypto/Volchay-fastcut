// Common Windows headers, kept in one place so we control the include order
// (some Windows SDK headers are very order-sensitive).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <combaseapi.h>

// Direct3D 11 / DXGI
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d3dcompiler.h>

// DirectComposition
#include <dcomp.h>

// DirectWrite + Direct2D (used for some custom drawing).
#include <d2d1_1.h>
#include <dwrite.h>

// Media Foundation
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <optional>
#include <chrono>

namespace volchay {

// Convert UTF-8 to UTF-16.
inline std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// Convert UTF-16 to UTF-8.
inline std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                          out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace volchay
