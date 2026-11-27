// the log's opening lines: what the game runs on and with which settings
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <intrin.h>
#include <objbase.h>
#include <winioctl.h>
// defines (not only declares) PKEY_AudioEndpoint_FormFactor
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "image_symbols.h"

#pragma comment(lib, "advapi32.lib")

namespace app {

namespace {

std::string RegString(HKEY key, const char* name) {
    char text[256] = {};
    DWORD size = sizeof(text) - 1;
    if (RegQueryValueExA(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(text), &size) != ERROR_SUCCESS) return {};
    return text;
}

std::string OsVersion() {
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ | KEY_WOW64_64KEY,
                      &key) != ERROR_SUCCESS)
        return "unknown";
    std::string name = RegString(key, "ProductName"), display = RegString(key, "DisplayVersion"),
                build = RegString(key, "CurrentBuildNumber");
    DWORD ubr = 0, size = sizeof(ubr);
    RegQueryValueExA(key, "UBR", nullptr, nullptr, reinterpret_cast<LPBYTE>(&ubr), &size);
    RegCloseKey(key);
    // Windows 11 still names itself Windows 10 here, its build number tells
    if (auto ten = name.find("Windows 10"); ten != std::string::npos && std::atoi(build.c_str()) >= 22000)
        name.replace(ten, 10, "Windows 11");
    if (!display.empty()) name += " " + display;
    if (!build.empty()) name += " (build " + build + "." + std::to_string(ubr) + ")";
    using WineVersion = const char*(__cdecl*)();
    if (auto wine = reinterpret_cast<WineVersion>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"))))
        name += std::string(" on Wine ") + wine();
    return name;
}

std::string CpuName() {
    int r[4] = {};
    __cpuid(r, int(0x80000000));
    if (uint32_t(r[0]) < 0x80000004) return "unknown";
    char brand[49] = {};
    for (int i = 0; i < 3; i++) {
        __cpuid(r, int(0x80000002 + i));
        std::memcpy(brand + i * 16, r, 16);
    }
    std::string s = brand;
    s.erase(0, s.find_first_not_of(' '));
    s.erase(s.find_last_not_of(' ') + 1);
    return s;
}

std::string Utf8(const std::filesystem::path& p) {
    std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// " @ 144 Hz" for the main monitor's current mode, empty when Windows does not say
std::string Refresh() {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) return {};
    return " @ " + std::to_string(dm.dmDisplayFrequency) + " Hz";
}

// the default output's shared-mode mix format and form factor; never its name, which players often set to their own
std::string AudioOutput() {
    HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string out;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) &&
        SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        IAudioClient* client = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client)))) {
            WAVEFORMATEX* fmt = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&fmt)) && fmt) {
                out = std::to_string(fmt->nChannels) + " ch, " + std::to_string(fmt->nSamplesPerSec) + " Hz";
                CoTaskMemFree(fmt);
            }
            client->Release();
        }
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_FormFactor, &v)) && v.vt == VT_UI4) {
                static const char* const kForms[] = {"remote network device", "speakers", "line level", "headphones",
                                                     "microphone", "headset", "handset", "digital pass-through",
                                                     "S/PDIF", "HDMI/DisplayPort"};
                out += (out.empty() ? "" : ", ") + std::string(v.ulVal < std::size(kForms) ? kForms[v.ulVal] : "other");
            }
            PropVariantClear(&v);
            props->Release();
        }
        if (out.empty()) out = "format unknown";
    } else {
        out = "no default output device";
    }
    if (dev) dev->Release();
    if (en) en->Release();
    if (SUCCEEDED(init)) CoUninitialize();
    return out;
}

// SSD or hard disk for the drive holding path; network, removable, spanned volumes and Wine say what they can
std::string DriveKind(const std::filesystem::path& path) {
    std::error_code ec;
    std::wstring abs = std::filesystem::absolute(path, ec).wstring();
    // the volume the path is on, also behind a folder mount point: C:\mount\x -> its own volume, not C:
    wchar_t mount[MAX_PATH] = {}, volume[64] = {};
    if (!GetVolumePathNameW(abs.c_str(), mount, MAX_PATH)) return "drive unknown";
    UINT type = GetDriveTypeW(mount);
    if (type == DRIVE_REMOTE) return "network drive";
    if (type == DRIVE_REMOVABLE) return "removable drive";
    if (!GetVolumeNameForVolumeMountPointW(mount, volume, 64)) return "drive unknown";
    std::wstring device = volume;  // \\?\Volume{guid}\ opened as \\.\Volume{guid}
    if (device.size() < 5 || device.back() != L'\\') return "drive unknown";
    device.pop_back();
    device[2] = L'.';
    HANDLE h = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "drive unknown";
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d{};
    DWORD got = 0;
    std::string kind = "drive unknown";
    if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q), &d, sizeof(d), &got, nullptr) &&
        got >= sizeof(d))
        kind = d.IncursSeekPenalty ? "hard disk" : "SSD";
    CloseHandle(h);
    return kind;
}

}  // namespace

void LogSystem() {
    std::time_t now = std::time(nullptr);
    char date[32] = {};
    std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    LOGR("%ls %s(%s), %s", kGame.title, kGame.version ? ("v" + std::string(kGame.version) + " ").c_str() : "",
         IMAGE_XBE, date);
    std::string exe = Utf8(ExeDir());
    LOGR("Path: %s (%s)", exe.c_str(), DriveKind(ExeDir()).c_str());
    LOGR("OS: %s", OsVersion().c_str());
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    LOGR("CPU: %s (%lu threads)", CpuName().c_str(), si.dwNumberOfProcessors);
    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    LOGR("RAM: %.1f GB (%.1f GB free)", mem.ullTotalPhys / 1073741824.0, mem.ullAvailPhys / 1073741824.0);
    // one line per adapter, each monitor reports its own
    std::vector<std::string> seen;
    for (DWORD i = 0;; i++) {
        DISPLAY_DEVICEA dd{sizeof(dd)};
        if (!EnumDisplayDevicesA(nullptr, i, &dd, 0)) break;
        if (std::find(seen.begin(), seen.end(), dd.DeviceString) != seen.end()) continue;
        seen.emplace_back(dd.DeviceString);
        LOGR("GPU %zu: %s%s", seen.size(), dd.DeviceString, (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " (primary)" : "");
    }
    Mode desk = Desktop();
    LOGR("Desktop: %ux%u%s", desk.width, desk.height, Refresh().c_str());
    LOGR("Audio output: %s", AudioOutput().c_str());
    // the game's own files: a disc image or folder somewhere else, or the assets folder beside the exe
    const std::string& game = Get().game_path;
    std::filesystem::path where = game.empty() ? DefaultGamePath()
                                               : std::filesystem::path(reinterpret_cast<const char8_t*>(game.c_str()));
    if (!where.empty()) LOGR("Game files: %s", DriveKind(where).c_str());
    LogSettings();
}

}  // namespace app
