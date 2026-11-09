#include "util.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace {
std::mutex gLogMutex;
FILE* gLog = nullptr;
constexpr long kMaxLogBytes = 8 * 1024 * 1024;

std::wstring logPath() { return logDirectory() + L"\\host.log"; }

void openLog() {
    std::wstring path = logPath();
    gLog = _wfopen(path.c_str(), L"a");
    if (!gLog) return;
    // Past the size limit, keep one previous file and start fresh.
    fseek(gLog, 0, SEEK_END);
    if (ftell(gLog) > kMaxLogBytes) {
        fclose(gLog);
        std::wstring old = path + L".1";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        gLog = _wfopen(path.c_str(), L"a");
    }
}
}  // namespace

std::wstring logDirectory() {
    wchar_t* base = nullptr;
    std::wstring dir = L".";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\TDisplay";
        CoTaskMemFree(base);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void logInit() {
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (!gLog) openLog();
}

void logToConsole() {
    std::lock_guard<std::mutex> lock(gLogMutex);
    gLog = stdout;
}

void logLine(const char* format, ...) {
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    std::lock_guard<std::mutex> lock(gLogMutex);
    if (!gLog) openLog();
    if (!gLog) return;
    fprintf(gLog, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            t.wMilliseconds, text);
    fflush(gLog);
}

uint64_t nowUs() {
    static const uint64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t ticks = static_cast<uint64_t>(c.QuadPart);
    return ticks / frequency * 1000000 + ticks % frequency * 1000000 / frequency;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::wstring exeDirectory() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n);
    size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}
