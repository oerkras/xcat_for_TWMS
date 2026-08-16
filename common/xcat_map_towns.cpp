#include "xcat_map_towns.h"

#include <Windows.h>

#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>

namespace xcat {
namespace {

std::mutex gMu;
std::string gLoadedBin;
std::unordered_set<int> gTownIds;
bool gTried = false;

std::string JoinPath(const char* bin, const char* rel) {
    std::string out = bin ? bin : "";
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out.push_back('\\');
    out += rel ? rel : "";
    return out;
}

void EnsureLoaded(const char* payloadBinDir) {
    std::lock_guard<std::mutex> lock(gMu);
    const std::string bin = payloadBinDir ? payloadBinDir : "";
    if (gTried && gLoadedBin == bin) return;
    gTried = true;
    gLoadedBin = bin;
    gTownIds.clear();
    if (bin.empty()) return;

    const std::string path = JoinPath(bin.c_str(), "dataservice\\map_info.tsv");
    std::ifstream f(path, std::ios::binary);
    if (!f) return;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t t0 = line.find('\t');
        if (t0 == std::string::npos) continue;
        const int mapId = atoi(line.c_str());
        const size_t t1 = line.find('\t', t0 + 1);
        const size_t t2 = line.find('\t', t1 == std::string::npos ? line.size() : t1 + 1);
        const size_t t3 = line.find('\t', t2 == std::string::npos ? line.size() : t2 + 1);
        const size_t t4 = line.find('\t', t3 == std::string::npos ? line.size() : t3 + 1);
        if (t3 == std::string::npos || t4 == std::string::npos) continue;
        const int town = atoi(line.c_str() + t3 + 1);
        if (town == 1 && mapId > 0) gTownIds.insert(mapId);
    }
}

}  // namespace

bool IsMapInfoTown(const char* payloadBinDir, int mapId) {
    if (mapId <= 0) return false;
    EnsureLoaded(payloadBinDir);
    std::lock_guard<std::mutex> lock(gMu);
    return gTownIds.count(mapId) != 0;
}

}  // namespace xcat
