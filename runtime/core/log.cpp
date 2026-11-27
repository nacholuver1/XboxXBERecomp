#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace logx {

#ifdef NDEBUG
const bool kVerbose = [] {
    const char* v = std::getenv("XBE_LOG");
    return v && *v && *v != '0';
}();
#endif

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static std::string g_path;
static auto g_start = std::chrono::steady_clock::now();

constexpr int kKeep = 30;          // debug: runs kept
constexpr int kKeepReleases = 3;   // release: earlier sessions kept

// <drive>:\Users\<name>\ and, under Wine/Proton, <drive>:\home\<name>\ (either slash, any case) get <user> for the
// name, so a log players post shows no Windows or Linux user name; other paths stay as they are
static std::string NoUserName(std::string text) {
    auto slash = [](char c) { return c == '\\' || c == '/'; };
    for (size_t at = 0; (at = text.find(':', at)) != std::string::npos; at++) {
        size_t dir = at + 1, len = 0;
        if (dir < text.size() && slash(text[dir])) {
            if (_strnicmp(text.c_str() + dir + 1, "Users", 5) == 0) len = 5;
            else if (_strnicmp(text.c_str() + dir + 1, "home", 4) == 0) len = 4;
        }
        if (!len || dir + len + 2 > text.size() || !slash(text[dir + len + 1])) continue;
        // the name runs to the next separator (names may hold quotes: O'Neil); a quote closing the line stays
        size_t name = dir + len + 2, end = text.find_first_of("\\/", name);
        if (end == std::string::npos) {
            end = text.size();
            while (end > name && (text[end - 1] == '\'' || text[end - 1] == '"')) end--;
        }
        if (end > name) text.replace(name, end - name, "<user>");
    }
    return text;
}

// the run number of prefix_NNN.log, 0 for another name
static int RunNumber(const fs::path& file, const std::string& prefix) {
    std::string name = file.filename().string();
    if (file.extension() != ".log" || name.size() <= prefix.size() + 1 || name.compare(0, prefix.size() + 1, prefix + "_"))
        return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}

void Init(const std::string& dir, const std::string& prefix, bool append) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!kVerbose) {
        g_path = (fs::path(dir) / (prefix + ".log")).string();
        // a new session keeps the last kKeepReleases ones as prefix.1.log (newest) to prefix.N.log
        if (!append && fs::exists(g_path, ec)) {
            auto old = [&](int i) { return fs::path(dir) / (prefix + "." + std::to_string(i) + ".log"); };
            fs::remove(old(kKeepReleases), ec);
            for (int i = kKeepReleases - 1; i >= 1; i--) fs::rename(old(i), old(i + 1), ec);
            fs::rename(g_path, old(1), ec);
            // not moved aside (open in an editor, say): carry on after it rather than wipe it
            if (ec) append = true;
        }
        g_file = std::fopen(g_path.c_str(), append ? "a" : "w");
        return;
    }
    std::vector<std::pair<int, fs::path>> runs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (int n = RunNumber(e.path(), prefix)) runs.emplace_back(n, e.path());
    std::sort(runs.begin(), runs.end());
    for (size_t i = 0; i + kKeep <= runs.size(); i++) fs::remove(runs[i].second, ec);
    char name[256];
    std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), runs.empty() ? 1 : runs.back().first + 1);
    g_path = (fs::path(dir) / name).string();
    g_file = std::fopen(g_path.c_str(), "w");
}

const std::string& Path() { return g_path; }

void Write(Level lv, const char* fmt, ...) {
    if (!kVerbose && lv < Level::Error) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    std::string text = NoUserName(msg);
    double t =std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    static const char* names[] = {"debug", "info", "warn", "error", "info"};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    std::lock_guard<std::mutex> lock(g_mutex);
    char line[4300];
    int n = kVerbose ? std::snprintf(line, sizeof(line), "[%9.3f] [%s] [t%04zx] %s\n", t, names[int(lv)], size_t(tid), text.c_str())
                     : std::snprintf(line, sizeof(line), "[%9.3f] %s%s\n", t, lv == Level::Error ? "error: " : "",
                                     text.c_str());
    n = std::min(n, int(sizeof(line)) - 1);
    if (g_file) {
        std::fwrite(line, 1, size_t(n), g_file);
        if (lv >= Level::Warn) std::fflush(g_file);
    }
    std::fwrite(line, 1, size_t(n), stderr);
}

void Flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fflush(g_file);
}

}  // namespace logx
