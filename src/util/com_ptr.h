// Minimal smart pointer for COM interfaces. We avoid pulling in WIL or ATL
// to keep MinGW cross-compilation friction free.
#pragma once

#include <utility>

namespace volchay {

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}
    explicit ComPtr(T* p) noexcept : p_(p) {
        if (p_) p_->AddRef();
    }
    ComPtr(const ComPtr& o) noexcept : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            if (p_) p_->AddRef();
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    void reset() noexcept {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

    T* get() const noexcept { return p_; }
    T* operator->() const noexcept { return p_; }
    T& operator*() const noexcept { return *p_; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    // Take ownership of an already AddRef'd raw pointer.
    void attach(T* p) noexcept {
        reset();
        p_ = p;
    }
    // Release ownership without calling Release.
    T* detach() noexcept {
        T* tmp = p_;
        p_ = nullptr;
        return tmp;
    }

    // Out-parameter slot for COM factory functions: caller fills *put().
    T** put() noexcept {
        reset();
        return &p_;
    }
    void** put_void() noexcept {
        reset();
        return reinterpret_cast<void**>(&p_);
    }

    // QueryInterface helper.
    template <typename U>
    long as(ComPtr<U>* out) const noexcept {
        return p_->QueryInterface(__uuidof(U), out->put_void());
    }

private:
    T* p_ = nullptr;
};

}  // namespace volchay
