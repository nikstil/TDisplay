// TDisplay's setup: one file that installs the laptop program and the virtual monitor driver, with nothing to choose.
// The program then puts the app on each tablet by itself. Run as uninstall.exe --uninstall it removes everything again.
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <dxgi.h>
#include <newdev.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <wincrypt.h>

#include <initguid.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <cstdio>
#include <cwchar>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "nvEncodeAPI.h"
#include "protocol.h"

namespace {

DEFINE_GUID(kEnumAdapterLuid, 0x1d39518c, 0xe220, 0x4da8, 0xa0, 0x7f, 0xba, 0x17, 0x25, 0x52, 0xd6, 0xb1);  // MFT_ENUM_ADAPTER_LUID

constexpr wchar_t kTitle[] = L"TDisplay Setup";
constexpr wchar_t kHostExe[] = L"tdisplay-host.exe";
constexpr wchar_t kHardwareId[] = L"Root\\MttVDD";
constexpr GUID kDisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
// The driver reads its settings from this fixed place.
constexpr wchar_t kDriverFolder[] = L"C:\\VirtualDisplayDriver";
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\TDisplay";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kSettingsKey[] = L"Software\\TDisplay";
constexpr int kAdbServerPort = 15037;
constexpr DWORD kOldestWindowsBuild = 18362;  // Windows 10 version 1903, the oldest the driver's display model runs on.

constexpr UINT kStepMessage = WM_APP + 1;
constexpr UINT kDoneMessage = WM_APP + 2;
constexpr int kCloseButton = 100;

struct PayloadFile {
    int resource;
    const wchar_t* path;
};
// Matches setup.rc.
constexpr PayloadFile kPayload[] = {
    {101, L"tdisplay-host.exe"},
    {102, L"TDisplay.apk"},
    {103, L"adb\\adb.exe"},
    {104, L"adb\\AdbWinApi.dll"},
    {105, L"adb\\AdbWinUsbApi.dll"},
    {106, L"driver\\MttVDD.dll"},
    {107, L"driver\\MttVDD.inf"},
    {108, L"driver\\mttvdd.cat"},
    {109, L"driver\\vdd_settings.xml"},
    {110, L"Getting started.txt"},
    {111, L"licenses\\Virtual Display Driver license.txt"},
    {112, L"licenses\\adb notices.txt"},
};

HWND gWindow = nullptr;
HWND gStatus = nullptr;
HWND gBar = nullptr;
HWND gClose = nullptr;
HFONT gFont = nullptr;

void logStep(const std::wstring& line) {
    wchar_t folder[MAX_PATH];
    GetTempPathW(MAX_PATH, folder);
    FILE* file = _wfopen((std::wstring(folder) + L"TDisplay setup.log").c_str(), L"a");
    if (!file) return;
    SYSTEMTIME now;
    GetLocalTime(&now);
    fwprintf(file, L"%02d:%02d:%02d %ls\n", now.wHour, now.wMinute, now.wSecond, line.c_str());
    fclose(file);
}

std::wstring errorText(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = message ? message : L"";
    if (message) LocalFree(message);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    wchar_t number[32];
    swprintf(number, 32, L" (error 0x%08lx)", code);
    return text + number;
}

std::wstring knownFolder(const KNOWNFOLDERID& id) {
    wchar_t* path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) result = path;
    CoTaskMemFree(path);
    return result;
}

std::wstring installFolder() {
    return knownFolder(FOLDERID_ProgramFiles) + L"\\TDisplay";
}

std::wstring ownPath() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::wstring(path, n);
}

bool exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool writePayload(const std::wstring& folder, std::wstring* error) {
    for (const PayloadFile& file : kPayload) {
        HRSRC found = FindResourceW(nullptr, MAKEINTRESOURCEW(file.resource), MAKEINTRESOURCEW(10));
        HGLOBAL loaded = found ? LoadResource(nullptr, found) : nullptr;
        const void* bytes = loaded ? LockResource(loaded) : nullptr;
        if (!bytes) {
            *error = std::wstring(L"This setup file is damaged: ") + file.path + L" is missing from it.";
            return false;
        }
        const DWORD size = SizeofResource(nullptr, found);
        const std::wstring path = folder + L"\\" + file.path;
        SHCreateDirectoryExW(nullptr, path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
        HANDLE out = INVALID_HANDLE_VALUE;
        // Something that only just closed can hold a file for a moment longer.
        for (int attempt = 0; attempt < 20 && out == INVALID_HANDLE_VALUE; ++attempt) {
            out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE) Sleep(250);
        }
        DWORD written = 0;
        const bool ok = out != INVALID_HANDLE_VALUE && WriteFile(out, bytes, size, &written, nullptr) && written == size;
        const DWORD code = GetLastError();
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (!ok) {
            *error = L"Could not write " + path + L": " + errorText(code);
            return false;
        }
    }
    return true;
}

bool nvencIsCurrent() {
    using MaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);
    HMODULE encoder = LoadLibraryW(L"nvEncodeAPI64.dll");
    auto maxVersion = encoder ? reinterpret_cast<MaxVersionFn>(GetProcAddress(encoder, "NvEncodeAPIGetMaxSupportedVersion")) : nullptr;
    uint32_t offered = 0;
    if (maxVersion) maxVersion(&offered);
    if (encoder) FreeLibrary(encoder);
    return offered >= ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION);
}

// Whether Windows offers a hardware encoder for this codec on this graphics adapter that actually starts. Some are
// listed and then refuse, like NVIDIA's on a laptop whose other graphics drive the screen.
bool windowsEncoderWorks(HMODULE mf, const LUID& adapter, const GUID& codec) {
    using AttributesFn = HRESULT(WINAPI*)(IMFAttributes**, UINT32);
    using EnumerateFn = HRESULT(WINAPI*)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*, IMFAttributes*, IMFActivate***, UINT32*);
    auto createAttributes = reinterpret_cast<AttributesFn>(reinterpret_cast<void*>(GetProcAddress(mf, "MFCreateAttributes")));
    auto enumerate = reinterpret_cast<EnumerateFn>(reinterpret_cast<void*>(GetProcAddress(mf, "MFTEnum2")));
    IMFAttributes* filter = nullptr;
    if (!createAttributes || !enumerate || FAILED(createAttributes(&filter, 1))) return false;
    filter->SetBlob(kEnumAdapterLuid, reinterpret_cast<const UINT8*>(&adapter), sizeof(adapter));
    const MFT_REGISTER_TYPE_INFO wanted{MFMediaType_Video, codec};
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    if (FAILED(enumerate(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &wanted, filter, &found, &count))) count = 0;
    filter->Release();
    bool works = false;
    for (UINT32 i = 0; i < count; ++i) {
        IMFTransform* encoder = nullptr;
        if (!works && SUCCEEDED(found[i]->ActivateObject(__uuidof(IMFTransform), reinterpret_cast<void**>(&encoder)))) {
            works = true;
            encoder->Release();
            found[i]->ShutdownObject();
        }
        found[i]->Release();
    }
    CoTaskMemFree(found);
    return works;
}

// The video encoder decides whether TDisplay can work on this PC at all, and which graphics the virtual monitor has
// to live on: NVIDIA's own encoder where its driver is new enough, and otherwise one Windows offers, preferring HEVC
// to H.264, which is what older graphics are limited to. Returns what to tell the user, or nothing if all is well.
std::wstring checkPc(std::wstring* gpuName) {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) get(&version);
    if (version.dwBuildNumber != 0 && version.dwBuildNumber < kOldestWindowsBuild) {
        return L"TDisplay needs Windows 10 version 1903 or newer. Nothing was installed.";
    }

    struct Gpu {
        std::wstring name;
        LUID luid;
        bool nvidia;
    };
    std::vector<Gpu> gpus;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            adapter->Release();
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) gpus.push_back({desc.Description, desc.AdapterLuid, desc.VendorId == 0x10DE});
        }
        factory->Release();
    }

    std::wstring oldNvidia;
    for (const Gpu& gpu : gpus) {
        if (!gpu.nvidia) continue;
        if (nvencIsCurrent()) {
            *gpuName = gpu.name;
            return L"";
        }
        oldNvidia = gpu.name;
        break;
    }

    // Media Foundation isn't on every edition of Windows.
    HMODULE mf = LoadLibraryW(L"mfplat.dll");
    using StartupFn = HRESULT(WINAPI*)(ULONG, DWORD);
    using ShutdownFn = HRESULT(WINAPI*)();
    auto startup = mf ? reinterpret_cast<StartupFn>(reinterpret_cast<void*>(GetProcAddress(mf, "MFStartup"))) : nullptr;
    auto shutdown = mf ? reinterpret_cast<ShutdownFn>(reinterpret_cast<void*>(GetProcAddress(mf, "MFShutdown"))) : nullptr;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (startup && shutdown && SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE))) {
        for (const GUID& codec : {MFVideoFormat_HEVC, MFVideoFormat_H264}) {
            for (const Gpu& gpu : gpus) {
                if (gpuName->empty() && windowsEncoderWorks(mf, gpu.luid, codec)) *gpuName = gpu.name;
            }
        }
        shutdown();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (!gpuName->empty()) return L"";

    if (!oldNvidia.empty()) {
        return L"The driver for your " + oldNvidia + L" is too old for TDisplay. Install the latest one from nvidia.com/drivers, then run this setup again. Nothing was installed.";
    }
    std::wstring message = L"TDisplay needs graphics that can encode video, and Windows doesn't offer a video encoder for any on this PC. Installing the latest driver from the graphics maker's website may fix that. Nothing was installed.";
    if (!mf) message += L"\n\nThis edition of Windows also leaves out the Media Feature Pack, which TDisplay needs for Intel and AMD graphics. Add it in Settings > Apps > Optional features.";
    std::wstring found;
    for (const Gpu& gpu : gpus) found += L"\n    " + gpu.name;
    if (!found.empty()) message += L"\n\nGraphics found on this PC:" + found;
    return message;
}

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

int relaunchElevated(const wchar_t* arguments) {
    const std::wstring path = ownPath();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = path.c_str();
    info.lpParameters = arguments;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) return 1;  // Includes the user saying no to the prompt.
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
}

bool runHidden(std::wstring command) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return false;
    WaitForSingleObject(process.hProcess, 30000);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

// ------------------------------------------------------------------------------------- running copies

void askAdbServerToExit() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kAdbServerPort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s != INVALID_SOCKET && connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
        const char request[] = "0009host:kill";
        send(s, request, sizeof(request) - 1, 0);
        char ignored[4];
        recv(s, ignored, sizeof(ignored), 0);
    }
    if (s != INVALID_SOCKET) closesocket(s);
    WSACleanup();
}

void closeRunningCopies(const std::wstring& folder) {
    // The program closes properly when asked, which also takes its monitor off the desktop.
    for (HWND window = FindWindowW(L"TDisplayHostWindow", nullptr); window; window = FindWindowExW(nullptr, window, L"TDisplayHostWindow", nullptr)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    for (int i = 0; i < 40 && FindWindowW(L"TDisplayHostWindow", nullptr); ++i) Sleep(100);
    askAdbServerToExit();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        const bool isHost = _wcsicmp(entry.szExeFile, kHostExe) == 0;
        if (!isHost && _wcsicmp(entry.szExeFile, L"adb.exe") != 0) continue;
        HANDLE process = OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (!process) continue;
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        // Another program's adb is none of our business, and it's not in the way.
        const bool ours = isHost || (QueryFullProcessImageNameW(process, 0, path, &size) && _wcsnicmp(path, folder.c_str(), folder.size()) == 0);
        if (ours) {
            TerminateProcess(process, 0);
            WaitForSingleObject(process, 2000);
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
}

// ------------------------------------------------------------------------------------- the driver

// The certificate that signed a driver's catalog file, or null. It has to be asked for as a signed message: read as
// what it holds, a catalog comes back with no message to find the signer in.
PCCERT_CONTEXT catalogSigner(const std::wstring& catalog) {
    HCERTSTORE catalogStore = nullptr;
    HCRYPTMSG message = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, catalog.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED, CERT_QUERY_FORMAT_FLAG_ALL, 0, nullptr, nullptr, nullptr, &catalogStore, &message, nullptr)) {
        logStep(L"could not read the driver's catalog: " + errorText(GetLastError()));
        return nullptr;
    }
    PCCERT_CONTEXT signer = nullptr;
    DWORD size = 0;
    if (message && CryptMsgGetParam(message, CMSG_SIGNER_CERT_INFO_PARAM, 0, nullptr, &size) && size) {
        std::string info(size, '\0');
        if (CryptMsgGetParam(message, CMSG_SIGNER_CERT_INFO_PARAM, 0, info.data(), &size)) {
            signer = CertFindCertificateInStore(catalogStore, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, info.data(), nullptr);
        }
    }
    if (message) CryptMsgClose(message);
    if (catalogStore) CertCloseStore(catalogStore, 0);
    return signer;
}

// Windows asks before installing a driver from a publisher it hasn't been told to trust, and the one thing this
// setup promises is that nobody has to answer anything. So the driver's publisher is trusted only for the moment
// it takes to install, unless it was trusted already. The driver keeps working afterwards.
class PublisherTrust {
public:
    explicit PublisherTrust(const std::wstring& catalog) {
        PCCERT_CONTEXT signer = catalogSigner(catalog);
        if (!signer) {
            logStep(L"could not tell who signed the driver, so Windows may ask about it");
            return;
        }
        store_ = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"TrustedPublisher");
        if (store_) {
            PCCERT_CONTEXT existing = CertFindCertificateInStore(store_, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_EXISTING, signer, nullptr);
            if (existing) {
                CertFreeCertificateContext(existing);
                logStep(L"the driver's publisher is already trusted");
            } else if (!CertAddCertificateContextToStore(store_, signer, CERT_STORE_ADD_NEW, &added_)) {
                logStep(L"could not trust the driver's publisher: " + errorText(GetLastError()));
            } else {
                logStep(L"trusting the driver's publisher for the install");
            }
        }
        CertFreeCertificateContext(signer);
    }
    ~PublisherTrust() {
        if (added_ && CertDeleteCertificateFromStore(added_)) logStep(L"no longer trusting the driver's publisher");
        if (store_) CertCloseStore(store_, 0);
    }

private:
    HCERTSTORE store_ = nullptr;
    PCCERT_CONTEXT added_ = nullptr;
};

bool isDriverDevice(HDEVINFO set, SP_DEVINFO_DATA& device) {
    wchar_t ids[1024] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids), sizeof(ids) - sizeof(wchar_t) * 2, nullptr)) return false;
    for (const wchar_t* id = ids; *id; id += wcslen(id) + 1) {
        if (_wcsicmp(id, kHardwareId) == 0) return true;
    }
    return false;
}

bool driverDeviceExists() {
    HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    bool found = false;
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &device); ++i) found = isDriverDevice(set, device);
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

bool installDriver(const std::wstring& folder, bool* rebootNeeded, std::wstring* error) {
    const std::wstring inf = folder + L"\\driver\\MttVDD.inf";
    PublisherTrust trust(folder + L"\\driver\\mttvdd.cat");

    HDEVINFO set = SetupDiCreateDeviceInfoList(&kDisplayClass, nullptr);
    if (set == INVALID_HANDLE_VALUE) {
        *error = L"Could not prepare the virtual monitor: " + errorText(GetLastError());
        return false;
    }
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    wchar_t ids[64] = {};
    wcscpy_s(ids, kHardwareId);
    const DWORD idsSize = static_cast<DWORD>((wcslen(ids) + 2) * sizeof(wchar_t));
    const bool created = SetupDiCreateDeviceInfoW(set, L"Display", &kDisplayClass, L"Virtual Display Driver", nullptr, DICD_GENERATE_ID, &device) &&
                         SetupDiSetDeviceRegistryPropertyW(set, &device, SPDRP_HARDWAREID, reinterpret_cast<const BYTE*>(ids), idsSize) &&
                         SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &device);
    const DWORD createError = GetLastError();
    SetupDiDestroyDeviceInfoList(set);
    if (!created) {
        *error = L"Could not create the virtual monitor: " + errorText(createError);
        return false;
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf.c_str(), INSTALLFLAG_FORCE, &reboot)) {
        *error = L"Windows would not install the virtual monitor driver: " + errorText(GetLastError());
        return false;
    }
    *rebootNeeded = reboot != FALSE;
    return true;
}

void removeDriver() {
    std::wstring packages;
    HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, 0);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &device); ++i) {
        if (!isDriverDevice(set, device)) continue;
        HKEY key = SetupDiOpenDevRegKey(set, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
        if (key != INVALID_HANDLE_VALUE) {
            wchar_t name[64] = {};
            DWORD size = sizeof(name) - sizeof(wchar_t);
            if (RegQueryValueExW(key, L"InfPath", nullptr, nullptr, reinterpret_cast<BYTE*>(name), &size) == ERROR_SUCCESS) packages = name;
            RegCloseKey(key);
        }
        if (!SetupDiCallClassInstaller(DIF_REMOVE, set, &device)) logStep(L"could not remove the virtual monitor: " + errorText(GetLastError()));
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!packages.empty() && !SetupUninstallOEMInfW(packages.c_str(), SUOI_FORCEDELETE, nullptr)) {
        logStep(L"could not remove driver package " + packages + L": " + errorText(GetLastError()));
    }
}

std::string toUtf8(const std::wstring& text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// On a PC with two kinds of graphics the virtual monitor has to live on the one whose encoder TDisplay uses.
void writeDriverSettings(const std::wstring& folder, const std::wstring& gpuName) {
    CreateDirectoryW(kDriverFolder, nullptr);
    const std::wstring path = std::wstring(kDriverFolder) + L"\\vdd_settings.xml";
    // The program adds each new tablet's screen size to this file, so it has to be able to write here.
    runHidden(std::wstring(L"icacls \"") + kDriverFolder + L"\" /grant *S-1-5-11:(OI)(CI)M /Q");
    if (!exists(path)) CopyFileW((folder + L"\\driver\\vdd_settings.xml").c_str(), path.c_str(), TRUE);

    std::stringstream contents;
    contents << std::ifstream(path.c_str(), std::ios::binary).rdbuf();
    std::string text = contents.str();
    const std::string open = "<friendlyname>";
    const std::string close = "</friendlyname>";
    const size_t start = text.find(open);
    const size_t end = text.find(close);
    if (start == std::string::npos || end == std::string::npos) return;
    std::string name;
    for (char c : toUtf8(gpuName)) {
        if (c == '&') name += "&amp;";
        else if (c == '<') name += "&lt;";
        else name += c;
    }
    text.replace(start + open.size(), end - start - open.size(), name);
    std::ofstream(path.c_str(), std::ios::binary | std::ios::trunc) << text;
}

// A newly installed driver puts its monitor on the desktop straight away, with nothing to show on it.
void takeMonitorOffDesktop() {
    for (int attempt = 0; attempt < 50; ++attempt) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &device, 0); ++i) {
            if (!wcsstr(device.DeviceString, L"Virtual Display Driver") || !(device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
            DEVMODEW off{};
            off.dmSize = sizeof(off);
            off.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
            ChangeDisplaySettingsExW(device.DeviceName, &off, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
            return;
        }
        Sleep(100);
    }
}

// ------------------------------------------------------------------------------------- Windows' lists of programs

bool createShortcut(const std::wstring& link, const std::wstring& target, const std::wstring& description) {
    IShellLinkW* shell = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) return false;
    shell->SetPath(target.c_str());
    shell->SetDescription(description.c_str());
    shell->SetWorkingDirectory(target.substr(0, target.find_last_of(L'\\')).c_str());
    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)))) {
        ok = SUCCEEDED(file->Save(link.c_str(), TRUE));
        file->Release();
    }
    shell->Release();
    return ok;
}

void setString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void setNumber(HKEY key, const wchar_t* name, DWORD value) {
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

void registerProgram(const std::wstring& folder) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    setString(key, L"DisplayName", L"TDisplay");
    setString(key, L"DisplayVersion", L"1." + std::to_wstring(proto::kVersion) + L"." + std::to_wstring(proto::kAppRevision));
    setString(key, L"Publisher", L"TDisplay");
    setString(key, L"InstallLocation", folder);
    setString(key, L"DisplayIcon", folder + L"\\" + kHostExe);
    setString(key, L"UninstallString", L"\"" + folder + L"\\uninstall.exe\" --uninstall");
    setNumber(key, L"NoModify", 1);
    setNumber(key, L"NoRepair", 1);
    setNumber(key, L"EstimatedSize", 12000);
    RegCloseKey(key);
}

// ------------------------------------------------------------------------------------- the window

void post(UINT message, const std::wstring& text, int percent) {
    PostMessageW(gWindow, message, static_cast<WPARAM>(percent), reinterpret_cast<LPARAM>(new std::wstring(text)));
}

int scaled(int value) {
    return MulDiv(value, GetDpiForSystem(), 96);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case kStepMessage:
        case kDoneMessage: {
            std::wstring* text = reinterpret_cast<std::wstring*>(lparam);
            SetWindowTextW(gStatus, text->c_str());
            delete text;
            SendMessageW(gBar, PBM_SETPOS, wparam, 0);
            if (message == kDoneMessage) {
                ShowWindow(gBar, SW_HIDE);
                ShowWindow(gClose, SW_SHOW);
                SetFocus(gClose);
            }
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == kCloseButton) DestroyWindow(window);
            return 0;
        case WM_CLOSE:
            if (IsWindowVisible(gClose)) DestroyWindow(window);
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wparam), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

void createWindow(HINSTANCE instance, const wchar_t* title) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    gFont = CreateFontIndirectW(&metrics.lfMessageFont);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.lpszClassName = L"TDisplaySetupWindow";
    RegisterClassW(&windowClass);

    const int width = scaled(560);
    const int height = scaled(320);
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT area{0, 0, width, height};
    AdjustWindowRect(&area, style, FALSE);
    const int outerWidth = area.right - area.left;
    const int outerHeight = area.bottom - area.top;
    gWindow = CreateWindowExW(0, windowClass.lpszClassName, title, style, (GetSystemMetrics(SM_CXSCREEN) - outerWidth) / 2, (GetSystemMetrics(SM_CYSCREEN) - outerHeight) / 2,
                              outerWidth, outerHeight, nullptr, nullptr, instance, nullptr);
    const int margin = scaled(20);
    const int bottom = scaled(36);
    gStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, margin, margin, width - 2 * margin, height - 2 * margin - bottom - scaled(10), gWindow, nullptr, instance, nullptr);
    gBar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, margin, height - margin - scaled(20), width - 2 * margin, scaled(20), gWindow, nullptr, instance, nullptr);
    SendMessageW(gBar, PBM_SETRANGE32, 0, 100);
    gClose = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_TABSTOP | BS_DEFPUSHBUTTON, width - margin - scaled(100), height - margin - bottom + scaled(6), scaled(100), bottom - scaled(6), gWindow,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCloseButton)), instance, nullptr);
    SendMessageW(gStatus, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
    SendMessageW(gClose, WM_SETFONT, reinterpret_cast<WPARAM>(gFont), TRUE);
    ShowWindow(gWindow, SW_SHOW);
    SetForegroundWindow(gWindow);
}

int runWindow(HINSTANCE instance, const wchar_t* title, void (*work)()) {
    createWindow(instance, title);
    std::thread worker(work);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(gWindow, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    worker.join();
    return 0;
}

// ------------------------------------------------------------------------------------- install and uninstall

std::wstring finishedMessage(bool rebootNeeded) {
    std::wstring done = L"TDisplay is installed";
    done += rebootNeeded ? L". Restart this PC to finish, then start TDisplay from the Start menu.\n\n"
                         : L" and running. Look for the small tablet icon in the taskbar's tray.\n\n";
    done += L"To connect a tablet (Android 11 or newer):\n\n"
            L"1.  Turn on USB debugging on the tablet: in Settings > About tablet, tap \"Build number\" seven times, then in Developer options turn on USB debugging.\n\n"
            L"2.  Plug the tablet into this PC with a USB cable, and tap Allow on the tablet.\n\n"
            L"3.  The app installs and opens by itself, and the tablet becomes a second monitor.\n\n"
            L"These steps are also in the tray icon's menu, under \"How to connect a tablet\".";
    return done;
}

void installWork() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    logStep(L"==== installing version " + std::to_wstring(proto::kVersion));
    const std::wstring folder = installFolder();
    std::wstring gpuName;
    std::wstring error;
    bool rebootNeeded = false;

    const auto fail = [&](const std::wstring& message) {
        logStep(L"failed: " + message);
        post(kDoneMessage, L"Setup could not finish.\n\n" + message + L"\n\nDetails are in \"TDisplay setup.log\" in your temporary folder.", 0);
    };

    const std::wstring problem = checkPc(&gpuName);
    if (!problem.empty()) {
        fail(problem);
        CoUninitialize();
        return;
    }
    logStep(L"the virtual monitor goes on " + gpuName);

    post(kStepMessage, L"Closing TDisplay if it's running...", 5);
    closeRunningCopies(folder);

    post(kStepMessage, L"Copying files...", 20);
    SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
    if (!writePayload(folder, &error)) {
        fail(error);
        CoUninitialize();
        return;
    }
    const std::wstring uninstaller = folder + L"\\uninstall.exe";
    if (_wcsicmp(ownPath().c_str(), uninstaller.c_str()) != 0) CopyFileW(ownPath().c_str(), uninstaller.c_str(), FALSE);

    post(kStepMessage, L"Installing the virtual monitor driver...", 50);
    writeDriverSettings(folder, gpuName);
    if (driverDeviceExists()) {
        logStep(L"the virtual monitor is already installed");
    } else {
        if (!installDriver(folder, &rebootNeeded, &error)) {
            fail(error);
            CoUninitialize();
            return;
        }
        logStep(L"installed the virtual monitor driver");
        takeMonitorOffDesktop();
    }

    post(kStepMessage, L"Finishing...", 90);
    registerProgram(folder);
    const std::wstring host = folder + L"\\" + kHostExe;
    if (!createShortcut(knownFolder(FOLDERID_CommonPrograms) + L"\\TDisplay.lnk", host, L"Use an Android tablet as a second monitor")) logStep(L"could not create the Start menu shortcut");

    if (!rebootNeeded) {
        // Through Explorer, so that it runs as the signed-in user and not with this setup's administrator rights.
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + host + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }
    logStep(L"finished");
    post(kDoneMessage, finishedMessage(rebootNeeded), 100);
    CoUninitialize();
}

void uninstallWork() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    logStep(L"==== uninstalling");
    const std::wstring folder = installFolder();

    post(kStepMessage, L"Closing TDisplay...", 10);
    closeRunningCopies(folder);

    post(kStepMessage, L"Removing the virtual monitor...", 40);
    removeDriver();
    SHFILEOPSTRUCTW remove{};
    const std::wstring driverFolder = std::wstring(kDriverFolder) + L'\0';
    remove.wFunc = FO_DELETE;
    remove.pFrom = driverFolder.c_str();
    remove.fFlags = FOF_NO_UI;
    SHFileOperationW(&remove);

    post(kStepMessage, L"Removing TDisplay...", 80);
    DeleteFileW((knownFolder(FOLDERID_CommonPrograms) + L"\\TDisplay.lnk").c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kSettingsKey);
    HKEY run = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &run) == ERROR_SUCCESS) {
        RegDeleteValueW(run, L"TDisplay");
        RegCloseKey(run);
    }
    logStep(L"finished");
    post(kDoneMessage, L"TDisplay and its virtual monitor have been removed from this PC.\n\nThe app stays on any tablet you connected. To remove it there, uninstall TDisplay PC in the tablet's settings.", 100);
    CoUninitialize();
}

// This program is running from the folder it has to delete, so that waits until it has exited.
void removeFolderAfterExit(const std::wstring& folder) {
    const std::wstring command = L"cmd.exe /c ping -n 3 127.0.0.1 >nul & rmdir /s /q \"" + folder + L"\"";
    std::wstring line = command;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

// For checking a build without changing anything on the PC: a picture of the finished window.
void previewWork() {
    post(kDoneMessage, finishedMessage(false), 100);
}

int writePreview(HINSTANCE instance, const wchar_t* path) {
    createWindow(instance, kTitle);
    previewWork();
    MSG message;
    const ULONGLONG end = GetTickCount64() + 700;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Sleep(10);
    }
    RECT rect;
    GetWindowRect(gWindow, &rect);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    SelectObject(dc, bitmap);
    PrintWindow(gWindow, dc, PW_RENDERFULLCONTENT);
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = width;
    info.biHeight = height;
    info.biPlanes = 1;
    info.biBitCount = 24;
    const DWORD stride = ((width * 3 + 3) / 4) * 4;
    std::string pixels(stride * height, '\0');
    GetDIBits(dc, bitmap, 0, height, pixels.data(), reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS);
    FILE* file = _wfopen(path, L"wb");
    if (file) {
        BITMAPFILEHEADER header{};
        header.bfType = 0x4D42;
        header.bfOffBits = sizeof(header) + sizeof(info);
        header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
        fwrite(&header, sizeof(header), 1, file);
        fwrite(&info, sizeof(info), 1, file);
        fwrite(pixels.data(), 1, pixels.size(), file);
        fclose(file);
    }
    return 0;
}

// For checking a build without changing anything on the PC: what this PC would be told, and the payload.
int writeCheckReport(const wchar_t* path, const wchar_t* catalog) {
    std::wstring gpuName;
    const std::wstring problem = checkPc(&gpuName);
    FILE* file = _wfopen(path, L"w");
    if (!file) return 1;
    if (catalog) {
        wchar_t name[256] = L"(none found)";
        if (PCCERT_CONTEXT signer = catalogSigner(catalog)) {
            CertGetNameStringW(signer, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
            CertFreeCertificateContext(signer);
        }
        fwprintf(file, L"driver signed by: %ls\n", name);
    }
    fwprintf(file, L"version %u\ngraphics: %ls\nproblem: %ls\ninstall folder: %ls\ndriver device exists: %d\nelevated: %d\n", proto::kVersion, gpuName.c_str(), problem.c_str(), installFolder().c_str(),
             driverDeviceExists() ? 1 : 0, isElevated() ? 1 : 0);
    fclose(file);
    return 0;
}

// Without this, a crash just makes the window disappear, which looks a lot like it finished.
LONG WINAPI reportCrash(EXCEPTION_POINTERS* info) {
    wchar_t code[32];
    swprintf(code, 32, L"0x%08lx", info->ExceptionRecord->ExceptionCode);
    logStep(std::wstring(L"crashed with exception ") + code);
    MessageBoxW(nullptr, L"Setup stopped because of an unexpected problem, so TDisplay may not be installed properly.\n\nDetails are in \"TDisplay setup.log\" in your temporary folder.", kTitle, MB_OK | MB_ICONERROR);
    return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    SetUnhandledExceptionFilter(reportCrash);
    if (argc >= 3 && wcscmp(argv[1], L"--check") == 0) return writeCheckReport(argv[2], argc >= 4 ? argv[3] : nullptr);
    if (argc >= 3 && wcscmp(argv[1], L"--preview") == 0) return writePreview(instance, argv[2]);
    if (argc >= 3 && wcscmp(argv[1], L"--extract") == 0) {
        std::wstring error;
        return writePayload(argv[2], &error) ? 0 : 1;
    }

    if (argc >= 2 && wcscmp(argv[1], L"--uninstall") == 0) {
        if (!isElevated()) return relaunchElevated(L"--uninstall");
        if (MessageBoxW(nullptr, L"Remove TDisplay and its virtual monitor from this PC?", L"TDisplay", MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
        const int code = runWindow(instance, L"Removing TDisplay", uninstallWork);
        removeFolderAfterExit(installFolder());
        return code;
    }

    // Checked before asking for administrator rights, so nobody approves a prompt for nothing.
    std::wstring gpuName;
    const std::wstring problem = checkPc(&gpuName);
    if (!problem.empty()) {
        MessageBoxW(nullptr, problem.c_str(), kTitle, MB_OK | MB_ICONWARNING);
        return 1;
    }
    if (!isElevated()) return relaunchElevated(L"");
    return runWindow(instance, kTitle, installWork);
}
