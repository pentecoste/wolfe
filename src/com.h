#pragma once

#include <windows.h>

#include <stdexcept>
#include <utility>

namespace wolfe {

// Minimal owning pointer for COM interfaces.
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& other) : ptr_(other.ptr_) {
        if (ptr_) ptr_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ~ComPtr() { reset(); }
    ComPtr& operator=(ComPtr other) noexcept {
        std::swap(ptr_, other.ptr_);
        return *this;
    }

    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    // For out-parameters. Releases whatever was held before.
    T** put() {
        reset();
        return &ptr_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }

private:
    T* ptr_ = nullptr;
};

class ComError : public std::runtime_error {
public:
    ComError(HRESULT hr, const char* what);
    HRESULT code() const { return hr_; }

private:
    HRESULT hr_;
};

// Throws ComError when hr is a failure code.
void check(HRESULT hr, const char* what);

// Every thread that touches WASAPI needs one of these alive.
struct ComApartment {
    ComApartment();
    ~ComApartment();
};

// While one of these is alive, the current thread is scheduled as an audio
// thread, ahead of ordinary work. Only for the code that moves audio.
class AudioThreadPriority {
public:
    AudioThreadPriority();
    ~AudioThreadPriority();
    AudioThreadPriority(const AudioThreadPriority&) = delete;
    AudioThreadPriority& operator=(const AudioThreadPriority&) = delete;

private:
    HANDLE task_ = nullptr;
};

}  // namespace wolfe
