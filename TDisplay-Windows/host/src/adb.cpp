#include "adb.h"

#include <cfgmgr32.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <sstream>

#include "net.h"
#include "util.h"

namespace {

constexpr int kControlTimeoutMs = 4000;

bool sendRequest(SOCKET s, const std::string& request) {
    char prefix[5];
    snprintf(prefix, sizeof(prefix), "%04x", static_cast<unsigned>(request.size()));
    return sendAll(s, prefix, 4) && sendAll(s, request.data(), request.size());
}

// Reads adb's four-byte status. On FAIL, *error gets the message that follows.
bool readStatus(SOCKET s, std::string* error) {
    char status[4];
    if (!recvAll(s, status, 4)) {
        if (error) *error = "no reply from adb server";
        return false;
    }
    if (memcmp(status, "OKAY", 4) == 0) return true;
    std::string message;
    char length[5] = {};
    if (recvAll(s, length, 4)) {
        const size_t n = strtoul(length, nullptr, 16);
        message.resize(n);
        if (n && !recvAll(s, message.data(), n)) message.clear();
    }
    if (error) *error = message.empty() ? "adb request failed" : message;
    return false;
}

bool isUsbSerial(const std::string& serial) {
    return serial.find(':') == std::string::npos && serial.rfind("emulator-", 0) != 0 &&
           serial.find("_adb-tls-connect") == std::string::npos;
}

}  // namespace

Adb::Adb(std::wstring adbExe, int serverPort) : exe_(std::move(adbExe)), port_(serverPort) {}

SOCKET Adb::connectServer() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<u_short>(port_));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    setRecvTimeout(s, kControlTimeoutMs);
    setSendTimeout(s, kControlTimeoutMs);
    setNoDelay(s);
    return s;
}

bool Adb::runTool(const std::wstring& arguments, DWORD waitMs) {
    std::wstring command = L"\"" + exe_ + L"\" -P " + std::to_wstring(port_) + L" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe_.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        logLine("adb: could not run %s (error %lu)", narrow(exe_).c_str(), GetLastError());
        return false;
    }
    bool ok = true;
    if (waitMs) {
        WaitForSingleObject(process.hProcess, waitMs);
        DWORD code = 1;
        GetExitCodeProcess(process.hProcess, &code);
        ok = code == 0;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return ok;
}

bool Adb::ensureServer() {
    SOCKET probe = connectServer();
    if (probe != INVALID_SOCKET) {
        closesocket(probe);
        return true;
    }
    if (!runTool(L"start-server", 0)) return false;
    // The start-server command itself takes seconds to report back, well after the server is
    // listening, so watch for the port rather than waiting on the command.
    const uint64_t deadline = nowUs() + 15000000;
    while (nowUs() < deadline) {
        probe = connectServer();
        if (probe != INVALID_SOCKET) {
            closesocket(probe);
            logLine("adb: server started on port %d", port_);
            return true;
        }
        Sleep(50);
    }
    logLine("adb: server did not come up on port %d", port_);
    return false;
}

void Adb::killServer() {
    logLine("adb: restarting the server");
    SOCKET s = connectServer();
    if (s == INVALID_SOCKET) return;
    sendRequest(s, "host:kill");
    char ignored[4];
    recv(s, ignored, sizeof(ignored), 0);
    closesocket(s);
}

bool Adb::usbInterfacePresent() {
    // The interface class every Android device's adb function registers under.
    GUID adbInterface = {0xF72FE0D4, 0xCBCB, 0x407D, {0x88, 0x14, 0x9E, 0xD6, 0x73, 0xD0, 0xDD, 0x6B}};
    ULONG size = 0;
    if (CM_Get_Device_Interface_List_SizeW(&size, &adbInterface, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return false;
    return size > 1;  // An empty list is a single terminating character.
}

bool Adb::install(const std::string& serial, const std::wstring& apkPath, std::string* reason) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + exe_ + L"\" -P " + std::to_wstring(port_) + L" -s " + widen(serial) + L" install -r \"" + apkPath + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(exe_.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(writeEnd);
    std::string output;
    if (started) {
        char buffer[512];
        DWORD n = 0;
        while (ReadFile(readEnd, buffer, sizeof(buffer), &n, nullptr) && n > 0) output.append(buffer, n);
        WaitForSingleObject(process.hProcess, 120000);
    }
    CloseHandle(readEnd);
    DWORD code = 1;
    if (started) {
        GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    const bool ok = started && code == 0;
    logLine("adb: app install %s: %s", ok ? "succeeded" : "failed", output.c_str());
    if (!ok && reason) *reason = output;
    return ok;
}

bool Adb::listUsbDevices(std::vector<AdbDevice>& devices) {
    devices.clear();
    SOCKET s = connectServer();
    if (s == INVALID_SOCKET) return false;
    bool ok = sendRequest(s, "host:devices-l") && readStatus(s, nullptr);
    char length[5] = {};
    std::string text;
    if (ok && (ok = recvAll(s, length, 4))) {
        text.resize(strtoul(length, nullptr, 16));
        ok = text.empty() || recvAll(s, text.data(), text.size());
    }
    closesocket(s);
    if (!ok) return false;

    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        AdbDevice device;
        if (!(fields >> device.serial >> device.state) || !isUsbSerial(device.serial)) continue;
        std::string field;
        while (fields >> field) {
            if (field.rfind("model:", 0) == 0) device.model = field.substr(6);
        }
        devices.push_back(device);
    }
    return true;
}

SOCKET Adb::openService(const std::string& serial, const std::string& service, std::string* error) {
    SOCKET s = connectServer();
    if (s == INVALID_SOCKET) {
        if (error) *error = "adb server not reachable";
        return INVALID_SOCKET;
    }
    if (!sendRequest(s, "host:transport:" + serial) || !readStatus(s, error) || !sendRequest(s, service) ||
        !readStatus(s, error)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

SOCKET Adb::openAbstract(const std::string& serial, const std::string& name, std::string* error) {
    return openService(serial, "localabstract:" + name, error);
}

bool Adb::shell(const std::string& serial, const std::string& command, std::string* output) {
    std::string error;
    SOCKET s = openService(serial, "shell:" + command, &error);
    if (s == INVALID_SOCKET) {
        logLine("adb: shell failed: %s", error.c_str());
        return false;
    }
    char buffer[4096];
    int n;
    while ((n = recv(s, buffer, sizeof(buffer), 0)) > 0) {
        if (output) output->append(buffer, n);
    }
    closesocket(s);
    return true;
}
