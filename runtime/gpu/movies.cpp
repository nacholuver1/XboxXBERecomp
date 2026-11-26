#include "gpu/movies.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace gpu::movies {
namespace {

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

constexpr size_t kAhead = 4;                               // decoded frames kept ready
constexpr uint32_t kSeekGap = 15;                          // further ahead than this: seek instead of decoding through

// one upscaled movie file, decoded on a thread of its own in order, frames numbered from 1 as Bink does
class Decoder {
public:
    ~Decoder() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    // the file's first frame and length are known, or false
    bool Open(const fs::path& path, uint32_t width, uint32_t height, uint32_t frames) {
        path_ = path;
        std::unique_lock<std::mutex> lock(mu_);
        thread_ = std::thread([this, width, height, frames] { Run(width, height, frames); });
        cv_.wait(lock, [this] { return opened_ != 0; });
        // the first frame is drawn at once: wait for it a little
        cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return opened_ < 0 || !ready_.empty(); });
        return opened_ > 0;
    }

    // the frame for want once decoded; frames before it are dropped
    bool Take(uint32_t want, uint32_t have, Frame& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (want != want_) {
            want_ = want;
            cv_.notify_all();
        }
        while (!ready_.empty() && ready_.front().number < want) ready_.pop_front();
        cv_.notify_all();
        if (want == have) return false;
        if (ready_.empty()) {
            if (want != last_miss_) empty_++, last_miss_ = want;
            return false;
        }
        if (ready_.front().number != want) {
            if (want != last_miss_) ahead_++, last_miss_ = want, ahead_by_ = ready_.front().number - want;
            return false;
        }
        out = std::move(ready_.front());
        ready_.pop_front();
        taken_++;
        return true;
    }

    // frames handed to the renderer, and the file's name, for the log
    uint32_t Taken() {
        std::lock_guard<std::mutex> lock(mu_);
        return taken_;
    }
    std::string Misses() {
        std::lock_guard<std::mutex> lock(mu_);
        char s[256];
        std::snprintf(s, sizeof(s),
                      "%u not decoded yet, %u decoder ahead (last by %u); conversion %.1f ms a frame; %u reads at %.1f ms; "
                      "%u seeks",
                      empty_, ahead_, ahead_by_, converted_ ? convert_ms_ / converted_ : 0.0, reads_,
                      reads_ ? read_ms_ / reads_ : 0.0, seeks_);
        return s;
    }
    std::string Name() const { return path_.filename().string(); }

private:
    void Run(uint32_t width, uint32_t height, uint32_t frames) {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        if (!mf) LOGW("movies: Media Foundation did not start (an N edition of Windows without the Media Feature Pack?)");
        int ok = mf && Start(width, height, frames) ? 1 : -1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            opened_ = ok;
        }
        cv_.notify_all();
        if (ok > 0) Loop();
        reader_.Reset();
        if (mf) MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();
    }

    bool Start(uint32_t width, uint32_t height, uint32_t frames) {
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes(&attrs, 1);
        if (FAILED(MFCreateSourceReaderFromURL(path_.c_str(), attrs.Get(), &reader_))) {
            LOGW("movies: %s does not open", path_.string().c_str());
            return false;
        }
        reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
        ComPtr<IMFMediaType> want;
        MFCreateMediaType(&want);
        want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        // the decoder's own NV12, converted here: the source reader's RGB conversion was too slow to keep up
        want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (FAILED(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get()))) {
            LOGW("movies: %s cannot be decoded to NV12", path_.string().c_str());
            return false;
        }
        ComPtr<IMFMediaType> got;
        reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &got);
        MFGetAttributeSize(got.Get(), MF_MT_FRAME_SIZE, &w_, &h_);
        UINT32 num = 0, den = 0;
        MFGetAttributeRatio(got.Get(), MF_MT_FRAME_RATE, &num, &den);
        fps_ = num && den ? double(num) / den : 30000.0 / 1001.0;
        stride_ = MFGetAttributeUINT32(got.Get(), MF_MT_DEFAULT_STRIDE, w_);
        PROPVARIANT dur;
        PropVariantInit(&dur);
        uint32_t length = 0;
        if (SUCCEEDED(reader_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur)))
            length = uint32_t(std::llround(double(dur.uhVal.QuadPart) / 1e7 * fps_));
        PropVariantClear(&dur);
        // the same movie, made bigger: a file of another movie or cut short draws the original instead
        bool shape = w_ >= width && h_ >= height && uint64_t(w_) * height == uint64_t(h_) * width;
        if (!shape || (frames && (length + 1 < frames || length > frames + 1))) {
            LOGR("movies: %s is %ux%u with %u frames, the movie %ux%u with %u: not used", path_.string().c_str(), w_,
                 h_, length, width, height, frames);
            return false;
        }
        LOGR("movies: %s, %ux%u, %u frames at %.3f fps", path_.filename().string().c_str(), w_, h_, length, fps_);
        return true;
    }

    void Seek(uint32_t number) {
        PROPVARIANT pos;
        InitPropVariantFromInt64(int64_t(std::llround((number - 1) / fps_ * 1e7)), &pos);
        reader_->SetCurrentPosition(GUID_NULL, pos);
        PropVariantClear(&pos);
    }

    void Loop() {
        uint32_t next = 1;  // the number of the frame the reader gives next
        for (;;) {
            uint32_t want;
            {
                std::unique_lock<std::mutex> lock(mu_);
                // decode on while fewer than kAhead frames are ready, or want lies behind them
                cv_.wait(lock, [this, next] { return stop_ || ready_.size() < kAhead || Behind(next); });
                if (stop_) return;
                want = want_;
                if (want && (Behind(next) || want > next + kSeekGap)) {
                    if (seeks_ < 8) LOGR("movies: seek to %u from %u, %zu ready", want, next, ready_.size());
                    ready_.clear();
                    seeks_++;
                    Seek(want);
                    next = want;
                }
            }
            DWORD flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> sample;
            auto r0 = std::chrono::steady_clock::now();
            HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample);
            {
                std::lock_guard<std::mutex> lock(mu_);
                read_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
                reads_++;
            }
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                LOGW("movies: decoding %s failed", path_.filename().string().c_str());
                return;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                std::unique_lock<std::mutex> lock(mu_);
                // the movie may still seek back: wait until asked for a frame behind the end
                uint32_t end = next;
                cv_.wait(lock, [this, end] { return stop_ || Behind(end); });
                if (stop_) return;
                continue;
            }
            if (!sample) continue;
            uint32_t number = uint32_t(std::llround(double(ts) / 1e7 * fps_)) + 1;
            // a seek starts at the keyframe before want: frames before it neither count nor ask for another seek
            if (number < want) continue;
            next = number + 1;
            Frame f;
            auto t = std::chrono::steady_clock::now();
            if (!Copy(sample.Get(), f)) continue;
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
            f.number = number;
            std::lock_guard<std::mutex> lock(mu_);
            convert_ms_ += ms;
            converted_++;
            ready_.push_back(std::move(f));
            cv_.notify_all();
        }
    }

    // want lies before the oldest frame decoded, and is not the one just handed over: decode it again (mu_ held)
    bool Behind(uint32_t next) const {
        uint32_t oldest = next - uint32_t(ready_.size());
        return want_ && want_ + 1 < oldest;
    }

    // NV12 (BT.601, limited range, as FFmpeg writes it from RGB) to BGRX
    bool Copy(IMFSample* sample, Frame& f) {
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
        BYTE* p = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&p, nullptr, &len))) return false;
        // the contiguous NV12 layout: luma rows of the type's stride, then the chroma plane; the plane height (the
        // frame height, or the decoder's rounded-up one) is what the length says
        size_t pitch = stride_;
        size_t rows = pitch ? len / (pitch * 3 / 2) : 0;
        bool fits = pitch >= w_ && rows >= h_;
        if (fits) {
            f.width = w_;
            f.height = h_;
            f.bgrx.resize(size_t(w_) * 4 * h_);
            const uint8_t* uv_plane = p + pitch * rows;
            for (uint32_t y = 0; y < h_; y++) {
                const uint8_t* yr = p + size_t(y) * pitch;
                const uint8_t* uv = uv_plane + size_t(y / 2) * pitch;
                uint8_t* out = &f.bgrx[size_t(y) * w_ * 4];
                for (uint32_t x = 0; x < w_; x++) {
                    int c = 298 * (int(yr[x]) - 16) + 128;
                    int d = int(uv[x & ~1u]) - 128, e = int(uv[x | 1u]) - 128;
                    auto clamp = [](int v) { return uint8_t(v < 0 ? 0 : v > 0xFF00 ? 255 : v >> 8); };
                    out[x * 4 + 0] = clamp(c + 516 * d);
                    out[x * 4 + 1] = clamp(c - 100 * d - 208 * e);
                    out[x * 4 + 2] = clamp(c + 409 * e);
                    out[x * 4 + 3] = 255;
                }
            }
        }
        buf->Unlock();
        return fits;
    }

    fs::path path_;
    ComPtr<IMFSourceReader> reader_;
    UINT32 w_ = 0, h_ = 0;
    uint32_t stride_ = 0;
    double convert_ms_ = 0;
    uint32_t converted_ = 0;
    double read_ms_ = 0;
    uint32_t reads_ = 0, seeks_ = 0;
    double fps_ = 0;
    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    int opened_ = 0;  // 1 open, -1 failed
    bool stop_ = false;
    uint32_t want_ = 0;
    std::deque<Frame> ready_;
    uint32_t taken_ = 0;
    uint32_t empty_ = 0, ahead_ = 0, ahead_by_ = 0, last_miss_ = 0;
};

// once a movie ends: how many of its frames were drawn upscaled
void Report(const std::shared_ptr<Decoder>& d, uint32_t frames) {
    if (d)
        LOGR("movies: %s ended, %u of %u frames drawn upscaled; missed: %s", d->Name().c_str(), d->Taken(), frames,
             d->Misses().c_str());
}

struct Playing {
    uint32_t bink = 0;
    uint32_t width = 0, height = 0, frames = 0;
    std::shared_ptr<Decoder> decoder;  // null: no upscaled file for this movie
};

std::mutex g_mu;
Playing g_playing;

// the file name without folder and extension
std::string Stem(const std::string& path) {
    size_t slash = path.find_last_of("\\/:");
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    size_t dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

}  // namespace

void Opened(uint32_t bink, const char* path) {
    Playing p;
    p.bink = bink;
    p.width = kernel::R32(bink);
    p.height = kernel::R32(bink + 4);
    uint32_t frames = p.frames = kernel::R32(bink + 8);
    std::string name = Stem(std::string(path, strnlen(path, 260)));
    if (app::Get().ai_movies && !name.empty()) {
        fs::path file = app::ExeDir() / "movies" / "ai" / (name + ".mp4");
        std::error_code ec;
        if (fs::exists(file, ec)) {
            auto d = std::make_shared<Decoder>();
            if (d->Open(file, p.width, p.height, frames)) p.decoder = std::move(d);
        }
    }
    LOGR("movies: %s %ux%u, %u frames%s", name.c_str(), p.width, p.height, frames, p.decoder ? ", upscaled" : "");
    std::shared_ptr<Decoder> old;
    std::lock_guard<std::mutex> lock(g_mu);
    Report(g_playing.decoder, g_playing.frames);
    old = std::move(g_playing.decoder);
    g_playing = std::move(p);
}

void Closed(uint32_t bink) {
    std::shared_ptr<Decoder> old;
    std::lock_guard<std::mutex> lock(g_mu);
    if (bink != g_playing.bink) return;
    Report(g_playing.decoder, g_playing.frames);
    old = std::move(g_playing.decoder);
    g_playing = Playing{};
}

bool IsMovie(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_playing.decoder && g_playing.width == width && g_playing.height == height;
}

uint32_t Shown() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_playing.bink ? kernel::R32(g_playing.bink + 0xC) : 0;
}

bool Take(uint32_t have, Frame& out) {
    std::shared_ptr<Decoder> d;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        d = g_playing.decoder;
    }
    return d && d->Take(Shown(), have, out);
}

}  // namespace gpu::movies
