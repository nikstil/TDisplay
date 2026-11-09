#pragma once
#include <windows.h>

#include <cstdint>
#include <string>

// Writes a timestamped line to %LOCALAPPDATA%\TDisplay\host.log.
void logInit();
void logLine(const char* format, ...);
// For test tools: log lines go to the console instead.
void logToConsole();
std::wstring logDirectory();

// Microseconds on the QueryPerformanceCounter clock.
uint64_t nowUs();

std::string narrow(const std::wstring& text);
std::wstring widen(const std::string& text);

// The folder holding this executable.
std::wstring exeDirectory();

// Owning wrapper for COM pointers; mingw's wrl is avoided to keep the build small.
template <class T>
class Com {
public:
    Com() = default;
    Com(const Com& other) : p_(other.p_) { if (p_) p_->AddRef(); }
    Com& operator=(const Com& other) {
        if (this != &other) { reset(); p_ = other.p_; if (p_) p_->AddRef(); }
        return *this;
    }
    ~Com() { reset(); }
    void reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    // For out-parameters. Releases anything currently held.
    T** put() { reset(); return &p_; }
    template <class U>
    HRESULT as(Com<U>& out) const { return p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.put())); }

private:
    T* p_ = nullptr;
};
