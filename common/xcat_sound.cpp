#include "xcat_sound.h"

#include "xcat_sound_presets.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace xcat::sound {
namespace {

struct Engine {
    std::mutex              mtx;
    std::condition_variable cv;
    std::deque<std::vector<int16_t>> queue;
    std::thread             worker;
    std::atomic<bool>       stop{false};
    std::atomic<bool>       ready{false};
    std::atomic<bool>       cancelPlay{false};
    HWAVEOUT                activeHwo{nullptr};

    bool PlayPcmBlocking(const std::vector<int16_t>& pcm) {
        if (pcm.empty()) return false;
        cancelPlay.store(false, std::memory_order_release);

        HWAVEOUT hwo = nullptr;
        WAVEFORMATEX wfx{};
        wfx.wFormatTag     = WAVE_FORMAT_PCM;
        wfx.nChannels      = 1;
        wfx.nSamplesPerSec = presets::kSampleRate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign    = static_cast<WORD>(wfx.nChannels * wfx.wBitsPerSample / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        if (waveOutOpen(&hwo, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
            return false;

        WAVEHDR hdr{};
        hdr.lpData         = reinterpret_cast<LPSTR>(const_cast<int16_t*>(pcm.data()));
        hdr.dwBufferLength = static_cast<DWORD>(pcm.size() * sizeof(int16_t));
        if (waveOutPrepareHeader(hwo, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) {
            waveOutClose(hwo);
            return false;
        }
        if (waveOutWrite(hwo, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) {
            waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
            waveOutClose(hwo);
            return false;
        }
        {
            std::lock_guard lk(mtx);
            activeHwo = hwo;
        }
        // LiePass ~1.5s；留足余量，中途可被 cancelPlay / waveOutReset 打断。
        DWORD waitMs = 0;
        while ((hdr.dwFlags & WHDR_DONE) == 0) {
            if (cancelPlay.load(std::memory_order_acquire) || stop.load(std::memory_order_acquire)) {
                waveOutReset(hwo);
                break;
            }
            Sleep(1);
            if (++waitMs > 8000) break;
        }
        {
            std::lock_guard lk(mtx);
            if (activeHwo == hwo) activeHwo = nullptr;
        }
        waveOutUnprepareHeader(hwo, &hdr, sizeof(hdr));
        waveOutClose(hwo);
        return true;
    }

    void WorkerLoop() {
        ready.store(true);
        for (;;) {
            std::vector<int16_t> pcm;
            {
                std::unique_lock lk(mtx);
                cv.wait(lk, [&] { return stop.load() || !queue.empty(); });
                if (stop.load() && queue.empty()) break;
                pcm = std::move(queue.front());
                queue.pop_front();
            }
            PlayPcmBlocking(pcm);
        }
    }

    void Start() {
        if (ready.load()) return;
        stop.store(false);
        worker = std::thread([this] { WorkerLoop(); });
        while (!ready.load()) Sleep(1);
    }

    void Stop() {
        if (!worker.joinable()) return;
        {
            std::lock_guard lk(mtx);
            stop.store(true);
            cancelPlay.store(true, std::memory_order_release);
            queue.clear();
            if (activeHwo) waveOutReset(activeHwo);
        }
        cv.notify_all();
        worker.join();
        ready.store(false);
        std::lock_guard lk(mtx);
        queue.clear();
        activeHwo = nullptr;
    }

    void CancelAndClear() {
        std::lock_guard lk(mtx);
        queue.clear();
        cancelPlay.store(true, std::memory_order_release);
        if (activeHwo) waveOutReset(activeHwo);
    }

    void Enqueue(std::vector<int16_t> pcm) {
        if (pcm.empty()) return;
        Start();
        {
            std::lock_guard lk(mtx);
            if (queue.size() > 8) queue.pop_front();
            queue.push_back(std::move(pcm));
        }
        cv.notify_one();
    }

    void InterruptAndEnqueue(std::vector<int16_t> pcm) {
        if (pcm.empty()) {
            CancelAndClear();
            return;
        }
        Start();
        {
            std::lock_guard lk(mtx);
            queue.clear();
            queue.push_back(std::move(pcm));
            cancelPlay.store(true, std::memory_order_release);
            if (activeHwo) waveOutReset(activeHwo);
        }
        cv.notify_one();
    }
};

Engine g_engine;
Settings g_settings{};
std::mutex g_cacheMtx;
std::vector<int16_t> g_cache[static_cast<size_t>(Id::Count)];

std::vector<int16_t> PcmFor(Id id) {
    const size_t idx = static_cast<size_t>(id);
    if (idx >= static_cast<size_t>(Id::Count)) return {};
    {
        std::lock_guard lk(g_cacheMtx);
        if (!g_cache[idx].empty()) return g_cache[idx];
    }
    auto pcm = presets::Generate(id);
    {
        std::lock_guard lk(g_cacheMtx);
        g_cache[idx] = pcm;
    }
    return pcm;
}

std::vector<int16_t> ScaledPcm(Id id) {
    auto pcm = PcmFor(id);
    if (pcm.empty()) return pcm;
    const float vol = g_settings.enabled ? std::clamp(g_settings.volume, 0.f, 1.f) : 0.f;
    if (vol <= 0.f) return {};
    if (vol >= 0.999f) return pcm;
    for (auto& s : pcm) s = static_cast<int16_t>(static_cast<float>(s) * vol);
    return pcm;
}

bool PlayInternal(Id id, bool blocking) {
    const auto pcm = ScaledPcm(id);
    if (pcm.empty()) return !g_settings.enabled;
    if (blocking) return g_engine.PlayPcmBlocking(pcm);
    g_engine.Enqueue(pcm);
    return true;
}

struct NameEntry {
    const char* name;
    Id          id;
};

constexpr NameEntry kNames[] = {
    {"click", Id::UiClick},
    {"confirm", Id::UiConfirm},
    {"toggle", Id::UiToggle},
    {"error", Id::UiError},
    {"shutdown", Id::UiShutdown},
    {"build-ok", Id::BuildOk},
    {"build-fail", Id::BuildFail},
    {"success", Id::BuildOk},
    {"fail", Id::BuildFail},
    {"launch-ok", Id::LaunchOk},
    {"launch-fail", Id::LaunchFail},
    {"feature-ready", Id::FeatureReady},
    {"notify", Id::Notify},
    {"alarm", Id::Alarm},
    {"alarm-timeout", Id::AlarmTimeout},
    {"timeout", Id::AlarmTimeout},
    {"restriction-alarm", Id::RestrictionAlarm},
    {"restriction", Id::RestrictionAlarm},
    {"lie-pass", Id::LiePass},
    {"lie-ok", Id::LiePass},
    {"game-context", Id::GameContextOk},
    {"gc-ok", Id::GameContextOk},
};

Id IdFromName(const char* name) {
    if (!name || !name[0]) return Id::Count;
    for (const auto& e : kNames) {
        if (_stricmp(name, e.name) == 0) return e.id;
    }
    return Id::Count;
}

}  // namespace

void Init() { g_engine.Start(); }

void Shutdown() { g_engine.Stop(); }

void SetSettings(const Settings& s) { g_settings = s; }

Settings GetSettings() { return g_settings; }

const char* IdName(Id id) {
    switch (id) {
    case Id::UiClick: return "ui-click";
    case Id::UiConfirm: return "ui-confirm";
    case Id::UiToggle: return "ui-toggle";
    case Id::UiError: return "ui-error";
    case Id::UiShutdown: return "ui-shutdown";
    case Id::BuildOk: return "build-ok";
    case Id::BuildFail: return "build-fail";
    case Id::LaunchOk: return "launch-ok";
    case Id::LaunchFail: return "launch-fail";
    case Id::FeatureReady: return "feature-ready";
    case Id::Notify: return "notify";
    case Id::Alarm: return "alarm";
    case Id::AlarmTimeout: return "alarm-timeout";
    case Id::RestrictionAlarm: return "restriction-alarm";
    case Id::LiePass: return "lie-pass";
    case Id::GameContextOk: return "game-context";
    default: return "?";
    }
}

bool Play(Id id) { return PlayInternal(id, false); }

bool PlayAsync(Id id) { return Play(id); }

bool PlayInterrupt(Id id) {
    const auto pcm = ScaledPcm(id);
    if (pcm.empty()) {
        g_engine.CancelAndClear();
        return !g_settings.enabled;
    }
    g_engine.InterruptAndEnqueue(pcm);
    return true;
}

void CancelPlayback() { g_engine.CancelAndClear(); }

bool PlayBlocking(Id id) { return PlayInternal(id, true); }

bool PlaySilenceBlocking(int durationMs) {
    if (durationMs <= 0) return true;
    if (!g_settings.enabled) return true;
    int samples = durationMs * presets::kSampleRate / 1000;
    if (samples < 1) samples = 1;
    std::vector<int16_t> silence(static_cast<size_t>(samples), 0);
    return g_engine.PlayPcmBlocking(silence);
}

bool PlayNamed(const char* name) {
    const Id id = IdFromName(name);
    if (id == Id::Count) return false;
    return Play(id);
}

bool PlayNamedBlocking(const char* name) {
    const Id id = IdFromName(name);
    if (id == Id::Count) return false;
    return PlayBlocking(id);
}

}  // namespace xcat::sound
