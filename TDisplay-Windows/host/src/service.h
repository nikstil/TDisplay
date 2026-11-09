// The background loop: waits for the tablet on USB, runs sessions, and reconnects.
#pragma once
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

enum class LinkState { Waiting, Streaming, Problem };

class Service {
public:
    using StatusCallback = std::function<void(LinkState, const std::wstring&)>;

    explicit Service(StatusCallback onStatus);
    ~Service();

    void start();
    void stop();

private:
    void loop();
    void setStatus(LinkState state, const std::wstring& text);

    StatusCallback onStatus_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    HANDLE wake_;
    std::wstring lastStatus_;
};
