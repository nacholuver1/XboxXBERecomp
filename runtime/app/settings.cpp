// the settings file beside the exe
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <sstream>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "audio/audio.h"
#include "core/log.h"

namespace app {

namespace {

const char* kModes[] = {"fullscreen", "borderless", "windowed"};

std::filesystem::path File() { return ExeDir() / kGame.settings; }

}  // namespace

Settings& Get() {
    static Settings s;
    return s;
}

namespace {
// the defaults, as they were before the settings file first loaded
struct Defaults {
    bool taken = false;
    Settings settings;
    std::vector<uint32_t> options;  // kGame.options in order, a flag as 0 or 1
} g_defaults;
}  // namespace

void Load() {
    Settings& s = Get();
    if (!g_defaults.taken) {
        g_defaults.taken = true;
        g_defaults.settings = s;
        for (const Option& o : kGame.options) g_defaults.options.push_back(o.flag ? uint32_t(*o.flag) : *o.value);
    }
    toml::table t;
    try {
        t = toml::parse_file(File().string());
    } catch (...) {
        return;
    }
    std::string mode = t["display_mode"].value_or(std::string(kModes[uint32_t(s.display_mode)]));
    for (uint32_t i = 0; i < 3; i++)
        if (mode == kModes[i]) s.display_mode = DisplayMode(i);
    s.width = t["width"].value_or(s.width);
    s.height = t["height"].value_or(s.height);
    s.render_percent = t["render_percent"].value_or(s.render_percent);
    s.fps_cap = t["fps_cap"].value_or(s.fps_cap);
    s.vsync = t["vsync"].value_or(s.vsync);
    s.aniso = t["anisotropic_filtering"].value_or(s.aniso);
    s.fxaa = t["fxaa"].value_or(s.fxaa);
    s.fsr = t["fsr"].value_or(s.fsr);
    s.sharpening = std::min(t["sharpening"].value_or(s.sharpening), 100u);
    s.original_aspect = t["original_aspect"].value_or(s.original_aspect);
    s.fit_hud = t["fit_hud"].value_or(s.fit_hud);
    s.upscale_videos = t["upscale_videos"].value_or(s.upscale_videos);
    s.ai_movies = t["ai_movies"].value_or(s.ai_movies);
    s.custom_textures = t["custom_textures"].value_or(s.custom_textures);
    s.ai_textures = t["ai_textures"].value_or(s.ai_textures);
    s.dump_textures = t["dump_textures"].value_or(s.dump_textures);
    s.texture_dump_format = std::min(t["texture_dump_format"].value_or(s.texture_dump_format), 1u);
    s.join_ip = t["join_ip"].value_or(s.join_ip);
    s.game_path = t["game_path"].value_or(s.game_path);
    s.accepted_release = t["accepted_release"].value_or(s.accepted_release);
    s.audio_output = std::min(t["audio_output"].value_or(s.audio_output), audio::kOutputMono);
    s.language = std::clamp(t["language"].value_or(s.language), 1u, 8u);
    for (const Option& o : kGame.options) {
        if (o.flag) *o.flag = t[o.key].value_or(*o.flag);
        else *o.value = t[o.key].value_or(*o.value);
    }
    if (kGame.load) kGame.load(&t);
    audio::SetOutputMode(s.audio_output);
}

void ResetToDefaults(const std::vector<std::string>& keep) {
    if (!g_defaults.taken) return;
    Settings& s = Get();
    Settings d = g_defaults.settings;
    d.game_path = s.game_path;
    d.accepted_release = s.accepted_release;
    s = d;
    for (size_t i = 0; i < kGame.options.size() && i < g_defaults.options.size(); i++) {
        const Option& o = kGame.options[i];
        if (std::find(keep.begin(), keep.end(), o.key) != keep.end()) continue;
        if (o.flag) *o.flag = g_defaults.options[i] != 0;
        else *o.value = g_defaults.options[i];
    }
    audio::SetOutputMode(s.audio_output);
}

namespace {

toml::table Table() {
    const Settings& s = Get();
    toml::table t{{"display_mode", kModes[uint32_t(s.display_mode)]},
                  {"width", int64_t(s.width)},
                  {"height", int64_t(s.height)},
                  {"render_percent", int64_t(s.render_percent)},
                  {"fps_cap", int64_t(s.fps_cap)},
                  {"vsync", s.vsync},
                  {"anisotropic_filtering", int64_t(s.aniso)},
                  {"fxaa", s.fxaa},
                  {"fsr", s.fsr},
                  {"sharpening", int64_t(s.sharpening)},
                  {"original_aspect", s.original_aspect},
                  {"fit_hud", s.fit_hud},
                  {"upscale_videos", s.upscale_videos},
                  {"ai_movies", s.ai_movies},
                  {"custom_textures", s.custom_textures},
                  {"ai_textures", s.ai_textures},
                  {"dump_textures", s.dump_textures},
                  {"texture_dump_format", int64_t(s.texture_dump_format)},
                  {"join_ip", s.join_ip},
                  {"game_path", s.game_path},
                  {"accepted_release", s.accepted_release},
                  {"audio_output", int64_t(s.audio_output)},
                  {"language", int64_t(s.language)}};
    for (const Option& o : kGame.options) {
        if (o.flag) t.insert(o.key, *o.flag);
        else t.insert(o.key, int64_t(*o.value));
    }
    if (kGame.save) kGame.save(&t);
    return t;
}

}  // namespace

// written beside the real file and renamed over it, so a crash mid save keeps the old settings
void Save() {
    std::filesystem::path tmp = File();
    tmp += ".tmp";
    std::ofstream f(tmp);
    f << Table() << "\n";
    f.close();
    if (f) MoveFileExW(tmp.c_str(), File().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void LogSettings() {
    std::ostringstream text;
    text << Table();
    std::istringstream lines(text.str());
    LOGR("Settings:");
    for (std::string line; std::getline(lines, line);)
        if (!line.empty()) LOGR("  %s", line.c_str());
}

Mode Desktop() {
    HMONITOR mon = MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(mon, &mi) && EnumDisplaySettingsW(mi.szDevice, ENUM_REGISTRY_SETTINGS, &dm))
        return {dm.dmPelsWidth, dm.dmPelsHeight};
    return {uint32_t(GetSystemMetrics(SM_CXSCREEN)), uint32_t(GetSystemMetrics(SM_CYSCREEN))};
}

Mode Output() {
    const Settings& s = Get();
    if (s.width && s.height) return {s.width, s.height};
    return Desktop();
}

// Output for every frame: the desktop mode lookup reads the registry, so it runs again only when the resolution
// setting or the window's monitor changes
Mode CachedOutput() {
    struct Key {
        uint32_t width;
        uint32_t height;
        HMONITOR monitor;
        bool operator==(const Key&) const = default;
    };
    static std::mutex lock;
    static Key key{};
    static Mode mode{};
    const Settings& s = Get();
    Key now{s.width, s.height, MonitorFromWindow(static_cast<HWND>(Window()), MONITOR_DEFAULTTOPRIMARY)};
    std::lock_guard<std::mutex> guard(lock);
    if (!(now == key) || !mode.height) {
        key = now;
        mode = Output();
    }
    return mode;
}

// asked per frame and per HUD quad: CachedOutput costs a monitor lookup, the registry only on a change
bool Widescreen() {
    if (Get().original_aspect) return false;
    Mode m = CachedOutput();
    return m.width * 2 >= m.height * 3;  // 3:2 or wider
}

float PresentAspect() {
    Mode m = CachedOutput();
    if (!m.height || !Widescreen()) return 4.0f / 3.0f;
    return std::clamp(float(m.width) / float(m.height), 16.0f / 9.0f, 32.0f / 9.0f);
}

namespace {
std::mutex g_note_lock;
std::string g_note;
constexpr wchar_t kRelaunchNote[] = L"XBE_RELAUNCH_NOTE";
}  // namespace

void PostNote(const std::string& text, bool relaunch) {
    if (relaunch) {
        std::wstring w(text.begin(), text.end());  // ASCII notes
        SetEnvironmentVariableW(kRelaunchNote, w.c_str());
        return;
    }
    std::lock_guard<std::mutex> guard(g_note_lock);
    g_note = text;
}

std::string TakeNote() {
    static std::once_flag handed;  // a note the previous instance left for this one
    std::call_once(handed, [] {
        wchar_t w[256];
        DWORD n = GetEnvironmentVariableW(kRelaunchNote, w, 256);
        if (n > 0 && n < 256) {
            SetEnvironmentVariableW(kRelaunchNote, nullptr);  // not handed on again by itself
            std::lock_guard<std::mutex> guard(g_note_lock);
            for (DWORD i = 0; i < n; i++) g_note += char(w[i]);
        }
    });
    std::lock_guard<std::mutex> guard(g_note_lock);
    return std::exchange(g_note, {});
}

float RenderScale(uint32_t guest_height) {
    float h = CachedOutput().height * (Get().render_percent / 100.0f);
    return guest_height ? std::max(h / guest_height, 0.1f) : 1.0f;
}

}  // namespace app
