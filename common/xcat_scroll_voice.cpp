#include "xcat_scroll_voice.h"

#include "xcat_log.h"
#include "xcat_sound.h"
#include "xcat_sound_presets.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace xcat::sound {
namespace {

constexpr int kGapMs = 16;
constexpr int kDingGapMs = 90;
constexpr int kSilenceThresh = 480;
constexpr int kTrimPadMs = 10;

std::mutex gMu;
bool gTried = false;
std::unordered_map<std::string, std::vector<int16_t>> gFrag;
std::unordered_map<int, std::vector<std::string>> gMap;

std::string JoinPath(const std::string& dir, const char* file) {
    if (dir.empty()) return file ? file : "";
    std::string out = dir;
    if (out.back() != '\\' && out.back() != '/') out += '\\';
    out += file ? file : "";
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool ReadFileBytes(const std::string& path, std::string& out) {
    std::ifstream f(Utf8ToWide(path).c_str(), std::ios::binary);
    if (!f.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

void ResampleLinearTo44100(const int16_t* src, size_t nSrc, int srcRate, std::vector<int16_t>& dst) {
    const int dstRate = presets::kSampleRate;
    if (srcRate <= 0 || nSrc == 0) {
        dst.clear();
        return;
    }
    if (srcRate == dstRate) {
        dst.assign(src, src + nSrc);
        return;
    }
    const double ratio = static_cast<double>(dstRate) / static_cast<double>(srcRate);
    const size_t nDst = static_cast<size_t>(static_cast<double>(nSrc) * ratio + 0.5);
    if (nDst == 0 || nDst > static_cast<size_t>(dstRate) * 12u) {
        dst.clear();
        return;
    }
    dst.resize(nDst);
    const size_t last = nSrc - 1;
    for (size_t i = 0; i < nDst; ++i) {
        const double pos = static_cast<double>(i) / ratio;
        size_t i0 = static_cast<size_t>(pos);
        if (i0 > last) i0 = last;
        const size_t i1 = i0 < last ? i0 + 1 : i0;
        const double t = pos - static_cast<double>(i0);
        const double s0 = static_cast<double>(src[i0]);
        const double s1 = static_cast<double>(src[i1]);
        dst[i] = static_cast<int16_t>(s0 + (s1 - s0) * t);
    }
}

void TrimEdgeSilence(std::vector<int16_t>& pcm, int rate) {
    if (rate <= 0 || pcm.size() < 32) return;
    const int pad = rate * kTrimPadMs / 1000;
    size_t a = 0;
    while (a < pcm.size() && std::abs(static_cast<int>(pcm[a])) < kSilenceThresh) ++a;
    size_t b = pcm.size();
    while (b > a && std::abs(static_cast<int>(pcm[b - 1])) < kSilenceThresh) --b;
    if (a >= b) return;
    if (a > static_cast<size_t>(pad)) a -= static_cast<size_t>(pad);
    else a = 0;
    if (b + static_cast<size_t>(pad) < pcm.size()) b += static_cast<size_t>(pad);
    else b = pcm.size();
    if (a == 0 && b == pcm.size()) return;
    pcm.assign(pcm.begin() + static_cast<std::ptrdiff_t>(a), pcm.begin() + static_cast<std::ptrdiff_t>(b));
}

bool LoadWav16Mono(const std::string& path, std::vector<int16_t>& pcm) {
    std::string raw;
    if (!ReadFileBytes(path, raw) || raw.size() < 44) return false;
    const auto* p = reinterpret_cast<const unsigned char*>(raw.data());
    if (std::memcmp(p, "RIFF", 4) != 0 || std::memcmp(p + 8, "WAVE", 4) != 0) return false;

    size_t off = 12;
    const size_t n = raw.size();
    const unsigned char* dataPtr = nullptr;
    uint32_t dataBytes = 0;
    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    while (off + 8 <= n) {
        char id[5]{};
        std::memcpy(id, p + off, 4);
        uint32_t sz = 0;
        std::memcpy(&sz, p + off + 4, 4);
        const size_t body = off + 8;
        if (body + sz > n) break;
        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            std::memcpy(&fmt, p + body, 2);
            std::memcpy(&ch, p + body + 2, 2);
            std::memcpy(&rate, p + body + 4, 4);
            std::memcpy(&bits, p + body + 14, 2);
        } else if (std::memcmp(id, "data", 4) == 0) {
            dataPtr = p + body;
            dataBytes = sz;
        }
        off = body + sz + (sz & 1u);
    }
    if (!dataPtr || fmt != 1 || ch != 1 || bits != 16) return false;
    if (rate < 8000 || rate > 48000) return false;
    if (dataBytes < 2 || dataBytes / 2 > rate * 12u) return false;
    const size_t ns = dataBytes / 2;
    const auto* samples = reinterpret_cast<const int16_t*>(dataPtr);
    ResampleLinearTo44100(samples, ns, static_cast<int>(rate), pcm);
    if (pcm.empty()) return false;
    TrimEdgeSilence(pcm, presets::kSampleRate);
    return !pcm.empty();
}

void AppendSilence(std::vector<int16_t>& dst, int ms) {
    const int n = presets::kSampleRate * ms / 1000;
    if (n > 0) dst.insert(dst.end(), static_cast<size_t>(n), 0);
}

void AppendPcm(std::vector<int16_t>& dst, const std::vector<int16_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

bool SplitLines(const std::string& raw, std::vector<std::string>& lines) {
    lines.clear();
    size_t pos = 0;
    while (pos <= raw.size()) {
        const size_t nl = raw.find('\n', pos);
        std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (!line.empty() && line[0] != '#') lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return true;
}

}  // namespace

void LoadScrollVoice(const char* prefsBinDir) {
    std::lock_guard<std::mutex> lock(gMu);
    if (gTried) return;
    gTried = true;
    gFrag.clear();
    gMap.clear();

    std::string ds = "dataservice\\scroll_voice";
    if (prefsBinDir && prefsBinDir[0]) {
        ds = JoinPath(JoinPath(prefsBinDir, "dataservice"), "scroll_voice");
    }
    const std::string mapPath = JoinPath(ds, "map.tsv");
    std::string mapRaw;
    if (!ReadFileBytes(mapPath, mapRaw)) {
        xcat::log::Warn("Sound", "scroll voice map missing: %s", mapPath.c_str());
        return;
    }
    std::vector<std::string> lines;
    SplitLines(mapRaw, lines);
    std::vector<std::string> needed;
    for (const auto& line : lines) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        const std::string idStr = line.substr(0, tab);
        const std::string rest = line.substr(tab + 1);
        int id = 0;
        try {
            id = std::stoi(idStr);
        } catch (...) {
            continue;
        }
        if (id <= 0) continue;
        std::vector<std::string> frags;
        size_t p = 0;
        while (p <= rest.size()) {
            const size_t c = rest.find(',', p);
            std::string f = rest.substr(p, c == std::string::npos ? std::string::npos : c - p);
            if (!f.empty()) {
                frags.push_back(f);
                needed.push_back(std::move(f));
            }
            if (c == std::string::npos) break;
            p = c + 1;
        }
        if (!frags.empty()) gMap[id] = std::move(frags);
    }

    int loaded = 0;
    int miss = 0;
    std::unordered_map<std::string, int> seen;
    for (const auto& fid : needed) {
        if (!seen.insert({fid, 1}).second) continue;
        std::vector<int16_t> pcm;
        const std::string wav = JoinPath(ds, (fid + ".wav").c_str());
        if (!LoadWav16Mono(wav, pcm) || pcm.empty()) {
            ++miss;
            continue;
        }
        gFrag[fid] = std::move(pcm);
        ++loaded;
    }
    {
        std::vector<int16_t> pcm;
        const std::string wav = JoinPath(ds, "pick_ok.wav");
        if (LoadWav16Mono(wav, pcm) && !pcm.empty()) {
            gFrag["pick_ok"] = std::move(pcm);
            ++loaded;
        } else {
            ++miss;
        }
    }
    {
        std::vector<int16_t> pcm;
        const std::string wav = JoinPath(ds, "pick_ok_dart.wav");
        if (LoadWav16Mono(wav, pcm) && !pcm.empty()) {
            gFrag["pick_ok_dart"] = std::move(pcm);
            ++loaded;
        } else {
            ++miss;
        }
    }
    xcat::log::Info("Sound", "scroll voice loaded maps=%zu frags=%d miss=%d dir=%s", gMap.size(),
                    loaded, miss, ds.c_str());
}

bool PlayScrollDropAnnounce(int itemId) {
    std::vector<int16_t> out;
    {
        std::lock_guard<std::mutex> lock(gMu);
        const auto ding = presets::Generate(Id::ScrollDrop);
        AppendPcm(out, ding);
        AppendSilence(out, kDingGapMs);

        auto it = gMap.find(itemId);
        if (itemId > 0 && it != gMap.end()) {
            bool ok = true;
            std::vector<const std::vector<int16_t>*> parts;
            parts.reserve(it->second.size());
            for (const auto& fid : it->second) {
                const auto fit = gFrag.find(fid);
                if (fit == gFrag.end() || fit->second.empty()) {
                    ok = false;
                    break;
                }
                parts.push_back(&fit->second);
            }
            if (ok) {
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (i) AppendSilence(out, kGapMs);
                    AppendPcm(out, *parts[i]);
                }
            }
        }
    }
    if (out.empty()) return false;
    return PlayPcmInterrupt(std::move(out));
}

bool PlayPickupSuccessAnnounce(int itemId) {
    std::vector<int16_t> out;
    {
        std::lock_guard<std::mutex> lock(gMu);
        const char* fid = (itemId == 2070005) ? "pick_ok_dart" : "pick_ok";
        auto it = gFrag.find(fid);
        if (it == gFrag.end() || it->second.empty()) {
            it = gFrag.find("pick_ok");
            if (it == gFrag.end() || it->second.empty()) return false;
        }
        AppendPcm(out, it->second);
    }
    if (out.empty()) return false;
    return PlayPcmInterrupt(std::move(out));
}

}  // namespace xcat::sound
