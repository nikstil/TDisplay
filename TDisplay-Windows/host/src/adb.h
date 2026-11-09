// Talks to a private adb server that only this program uses.
//
// Running on our own port means other software on the laptop that bundles adb
// (DroidCam, emulators, Android Studio) can't kill our server by restarting
// theirs with a different version. Streams go straight through the server with
// no listening port, and only USB devices are ever selected.
#pragma once
#include <winsock2.h>

#include <string>
#include <vector>

struct AdbDevice {
    std::string serial;
    std::string state;  // "device", "unauthorized", "offline", ...
    std::string model;
};

class Adb {
public:
    Adb(std::wstring adbExe, int serverPort);

    // Starts the server if it isn't already answering.
    bool ensureServer();

    // Stops the server. The next ensureServer starts a fresh one, which looks at USB from scratch.
    void killServer();

    // True if Windows currently sees an adb USB interface, whether or not our server has it.
    static bool usbInterfacePresent();

    // Lists devices attached by cable. Wireless debugging devices are left out.
    bool listUsbDevices(std::vector<AdbDevice>& devices);

    // Runs a shell command on the device. Output is optional.
    bool shell(const std::string& serial, const std::string& command, std::string* output = nullptr);

    // Installs or updates an app on the device.
    // On failure, *reason gets the error adb reported.
    bool install(const std::string& serial, const std::wstring& apkPath, std::string* reason);

    // Opens a stream to an abstract Unix socket on the device.
    // Returns INVALID_SOCKET on failure and sets *error to adb's reason.
    SOCKET openAbstract(const std::string& serial, const std::string& name, std::string* error);

    // Opens a stream to any service on the device, such as "shell:ls".
    SOCKET openService(const std::string& serial, const std::string& service, std::string* error);

private:
    SOCKET connectServer();
    // Runs adb.exe with these arguments. With waitMs 0 it doesn't wait and reports only the launch.
    bool runTool(const std::wstring& arguments, DWORD waitMs);

    std::wstring exe_;
    int port_;
};
