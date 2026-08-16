#include "xcat_auto_skill.h"

#include "process_util.h"
#include "xcat_config_ini.h"
#include "xcat_item_catalog.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xcat {
namespace {

constexpr uint32_t kAutoSkillIniVersion = 1u;
constexpr char kSec[] = "auto_skill";

constexpr int kJob1[] = {100, 200, 300, 400, 500};
constexpr int kJob2Warrior[] = {110, 120, 130};
constexpr int kJob2Mage[] = {210, 220, 230};
constexpr int kJob2Archer[] = {310, 320};
constexpr int kJob2Thief[] = {410, 420};
constexpr int kJob2Pirate[] = {510, 520};

struct CatalogSkill {
    int id = 0;
    int job = 0;
    std::string name;
};

struct CatalogPack {
    bool loaded = false;
    std::vector<CatalogSkill> skills;
    std::unordered_map<int, std::string> jobLabel;
    std::unordered_map<int, int> maxObserved;
    std::unordered_set<int> inBook;
    std::unordered_set<int> invisible;
    std::unordered_set<int> psd;
    std::unordered_map<int, std::string> nameById;
};

CatalogPack gPack;
std::once_flag gOnce;
std::string gPackDir;
std::atomic<bool> gPackReady{false};

const CatalogPack* PeekCatalog() {
    if (!gPackReady.load(std::memory_order_acquire)) return nullptr;
    return &gPack;
}

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    return CreateDirectoryUtf8(JoinBinPath(binDir, "state"));
}

void CopyJobList(const int* src, size_t n, int* out, uint32_t* count) {
    uint32_t w = 0;
    for (size_t i = 0; i < n && w < 16; ++i) out[w++] = src[i];
    *count = w;
}

void CompactOrder(int* ids, int* tgt, uint32_t& count) {
    uint32_t w = 0;
    for (uint32_t i = 0; i < kAutoSkillOrderMax; ++i) {
        if (ids[i] <= 0) continue;
        ids[w] = ids[i];
        int t = tgt[i];
        if (t < 0) t = 0;
        if (t > kAutoSkillTargetMax) t = kAutoSkillTargetMax;
        tgt[w] = t;
        ++w;
    }
    for (uint32_t i = w; i < kAutoSkillOrderMax; ++i) {
        ids[i] = 0;
        tgt[i] = 0;
    }
    count = w;
}

std::string FormatCsv(const int* vals, uint32_t count, bool skipNonPositive) {
    std::string s;
    for (uint32_t i = 0; i < count && i < kAutoSkillOrderMax; ++i) {
        if (skipNonPositive && vals[i] <= 0) continue;
        if (!s.empty()) s += ',';
        char buf[16]{};
        snprintf(buf, sizeof(buf), "%d", vals[i]);
        s += buf;
    }
    return s;
}

void ParseCsv(const std::string& csv, int* vals, uint32_t* count, bool skipNonPositive, int maxAbs) {
    for (uint32_t i = 0; i < kAutoSkillOrderMax; ++i) vals[i] = 0;
    uint32_t n = 0;
    size_t pos = 0;
    while (pos < csv.size() && n < kAutoSkillOrderMax) {
        size_t comma = csv.find(',', pos);
        if (comma == std::string::npos) comma = csv.size();
        std::string tok = csv.substr(pos, comma - pos);
        while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
        while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
        if (!tok.empty()) {
            int v = atoi(tok.c_str());
            if (v < 0) v = 0;
            if (maxAbs > 0 && v > maxAbs) v = maxAbs;
            if (!skipNonPositive || v > 0) vals[n++] = v;
        }
        pos = comma + 1;
    }
    if (count) *count = n;
}

void SplitTab(const std::string& line, std::vector<std::string>& cols) {
    cols.clear();
    size_t pos = 0;
    while (pos <= line.size()) {
        const size_t tab = line.find('\t', pos);
        cols.push_back(line.substr(pos, tab == std::string::npos ? std::string::npos : tab - pos));
        if (tab == std::string::npos) break;
        pos = tab + 1;
    }
}

bool IsInternalSkillName(const std::string& name) {
    if (name.empty()) return true;
    if (name.size() >= 4 && name[0] == '[' &&
        (name[1] == 'T' || name[1] == 't') &&
        (name[2] == 'W' || name[2] == 'w') && name[3] == ']') {
        return true;
    }
    return false;
}

bool ReadFileUtf8(const std::string& path, std::string& out) {
    std::ifstream f{std::filesystem::path(path), std::ios::binary};
    if (!f.is_open()) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);
    return true;
}

void LoadCatalog(const char* binDir, CatalogPack& pack) {
    pack = {};
    const std::string ds = ResolveDataServiceDir(binDir);
    std::string raw;
    std::vector<std::string> cols;

    if (ReadFileUtf8(ds + "\\skillbook_names.tsv", raw)) {
        size_t pos = 0;
        while (pos <= raw.size()) {
            const size_t nl = raw.find('\n', pos);
            std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty() && line[0] != '#') {
                SplitTab(line, cols);
                if (cols.size() >= 2 && !cols[1].empty()) {
                    const int job = atoi(cols[0].c_str());
                    if (job > 0) pack.jobLabel[job] = cols[1];
                }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    raw.clear();
    if (ReadFileUtf8(ds + "\\skill_meta.tsv", raw)) {
        size_t pos = 0;
        while (pos <= raw.size()) {
            const size_t nl = raw.find('\n', pos);
            std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty() && line[0] != '#') {
                SplitTab(line, cols);
                if (!cols.empty()) {
                    const int id = atoi(cols[0].c_str());
                    if (id > 0) {
                        pack.inBook.insert(id);
                        if (cols.size() >= 4 && !cols[3].empty()) {
                            const int mx = atoi(cols[3].c_str());
                            if (mx > 0) pack.maxObserved[id] = mx;
                        }
                        if (cols.size() >= 6 && cols[5] == "1") pack.psd.insert(id);
                        if (cols.size() >= 7 && cols[6] == "1") pack.invisible.insert(id);
                    }
                }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    raw.clear();
    if (ReadFileUtf8(ds + "\\skill_catalog_full.tsv", raw)) {
        size_t pos = 0;
        while (pos <= raw.size()) {
            const size_t nl = raw.find('\n', pos);
            std::string line = raw.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty() && line[0] != '#') {
                SplitTab(line, cols);
                if (cols.size() >= 3) {
                    const int id = atoi(cols[0].c_str());
                    const int job = atoi(cols[2].c_str());
                    if (id > 0 && AutoSkillJobFamily(job) != 0) {
                        if (pack.inBook.find(id) == pack.inBook.end()) continue;
                        if (pack.invisible.find(id) != pack.invisible.end()) continue;
                        if (pack.psd.find(id) != pack.psd.end()) continue;
                        const std::string name = cols.size() >= 2 ? cols[1] : "";
                        if (IsInternalSkillName(name)) continue;
                        CatalogSkill s;
                        s.id = id;
                        s.job = job;
                        s.name = name;
                        pack.skills.push_back(std::move(s));
                        if (pack.nameById.find(id) == pack.nameById.end() && !name.empty()) {
                            pack.nameById[id] = name;
                        }
                    }
                }
            }
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
    }

    pack.loaded = !pack.skills.empty();
}

const CatalogPack& Pack(const char* binDir) {
    const char* dir = binDir ? binDir : "";
    std::call_once(gOnce, [&] {
        gPackDir = dir;
        LoadCatalog(dir, gPack);
        gPackReady.store(gPack.loaded, std::memory_order_release);
    });
    if (gPackDir != dir) {
        gPackDir = dir;
        LoadCatalog(dir, gPack);
        gPackReady.store(gPack.loaded, std::memory_order_release);
    }
    return gPack;
}

const char* FallbackJobLabel(int job) {
    switch (job) {
        case 100: return "劍士";
        case 110: return "狂戰士";
        case 120: return "見習騎士";
        case 130: return "槍騎兵";
        case 200: return "法師";
        case 210: return "巫師(火/毒)";
        case 220: return "巫師(冰/雷)";
        case 230: return "僧侶";
        case 300: return "弓箭手";
        case 310: return "獵人";
        case 320: return "弩弓手";
        case 400: return "盜賊";
        case 410: return "刺客";
        case 420: return "俠盜";
        case 500: return "海盜";
        case 510: return "打手";
        case 520: return "槍手";
        default: return nullptr;
    }
}

struct FallbackSkill {
    int id;
    const char* name;
    int maxLv;
};

const FallbackSkill* FallbackSkillOf(int skillId) {
    static const FallbackSkill kFb[] = {
        {1000000, "生命淨化", 16},
        {1000001, "生命擴展", 10},
        {1000002, "生命恢復", 8},
        {1001003, "自身強化", 20},
        {1001004, "魔天一擊", 20},
        {1001005, "劍氣縱橫", 20},
        {1100000, "精準之劍", 20},
        {1100001, "精準之斧", 20},
        {1100002, "終極之劍", 30},
        {1100003, "終極之斧", 30},
        {1101004, "快速之劍", 20},
        {1101005, "快速之斧", 20},
        {1101006, "激勵", 20},
        {1101007, "反射之盾", 30},
        {1200000, "精準之劍", 20},
        {1200001, "精準之棍", 20},
        {1200002, "終極之劍", 30},
        {1200003, "終極之棍", 30},
        {1201004, "快速之劍", 20},
        {1201005, "快速之棍", 20},
        {1201006, "降魔咒", 20},
        {1201007, "反射之盾", 30},
        {1300000, "精準之槍", 20},
        {1300001, "精準之矛", 20},
        {1300002, "終極之槍", 30},
        {1300003, "終極之矛", 30},
        {1301004, "快速之槍", 20},
        {1301005, "快速之矛", 20},
        {1301006, "禦魔陣", 20},
        {1301007, "神聖之火", 30},
        {2000000, "魔力淨化", 16},
        {2000001, "魔力擴展", 10},
        {2001002, "魔心防禦", 20},
        {2001003, "魔力之盾", 20},
        {2001004, "魔靈彈", 20},
        {2001005, "魔力爪", 20},
        {2100000, "魔力吸收", 20},
        {2101001, "精神強化", 20},
        {2101002, "瞬間移動", 20},
        {2101003, "緩速術", 20},
        {2101004, "火焰箭", 30},
        {2101005, "毒霧", 30},
        {2200000, "魔力吸收", 20},
        {2201001, "精神強化", 20},
        {2201002, "瞬間移動", 20},
        {2201003, "緩速術", 20},
        {2201004, "冰錐術", 30},
        {2201005, "電閃雷鳴", 30},
        {2300000, "魔力吸收", 20},
        {2301001, "瞬間移動", 20},
        {2301002, "群體治癒", 30},
        {2301003, "神聖之光", 20},
        {2301004, "天使祝福", 20},
        {2301005, "神聖之箭", 30},
        {3000000, "精準強化", 16},
        {3000001, "霸王箭", 20},
        {3000002, "百步穿楊", 8},
        {3001003, "集中術", 20},
        {3001004, "斷魂箭", 20},
        {3001005, "二連箭", 20},
        {3100000, "精準之弓", 20},
        {3100001, "終極之弓", 30},
        {3101002, "快速之弓", 20},
        {3101003, "強弓", 20},
        {3101004, "無形之箭", 20},
        {3101005, "炸彈箭", 30},
        {3200000, "精準之弩", 20},
        {3200001, "終極之弩", 30},
        {3201002, "快速之弩", 20},
        {3201003, "強弩", 20},
        {3201004, "無形之箭", 20},
        {3201005, "穿透之箭", 30},
        {4000000, "幻化術", 20},
        {4000001, "鷹之眼", 8},
        {4001002, "詛咒術", 20},
        {4001003, "隱身術", 20},
        {4001334, "劈空斬", 20},
        {4001344, "雙飛斬", 20},
        {4100000, "精準暗器", 20},
        {4100001, "強力投擲", 30},
        {4100002, "恢復術", 20},
        {4101003, "極速暗殺", 20},
        {4101004, "速度激發", 20},
        {4101005, "吸血術", 30},
        {4200000, "精準之刀", 20},
        {4200001, "恢復術", 20},
        {4201002, "快速之刀", 20},
        {4201003, "速度激發", 20},
        {4201004, "妙手術", 30},
        {4201005, "迴旋斬", 30},
        {5000000, "極限迴避", 20},
        {5001001, "衝擊拳", 20},
        {5001002, "旋風斬", 20},
        {5001003, "雙子星攻擊", 20},
        {5001005, "衝鋒", 10},
        {5100000, "體魄強健", 10},
        {5100001, "精通指虎", 20},
        {5101002, "迴旋肘擊", 20},
        {5101003, "昇龍拳", 20},
        {5101004, "狂暴衝擊", 20},
        {5101005, "魔力再生", 10},
        {5101006, "致命快打", 20},
        {5101007, "偽裝術", 10},
        {5200000, "精通槍法", 20},
        {5201001, "散射", 20},
        {5201002, "炸彈投擲", 20},
        {5201003, "迅雷再起", 20},
        {5201004, "偽裝射擊", 20},
        {5201005, "緩降術", 10},
        {5201006, "脫離戰場", 20},
    };
    for (const auto& s : kFb) {
        if (s.id == skillId) return &s;
    }
    return nullptr;
}

bool ReadAutoSkillIni(const char* binDir, AutoSkillConfig& out, uint64_t* outWriteTick) {
    if (outWriteTick) *outWriteTick = 0;
    if (!binDir || !binDir[0]) return false;
    IniStore ini{};
    const std::string path = UserConfigIniPath(binDir);
    if (!LoadIniFile(path.c_str(), ini)) return false;
    uint32_t version = 0;
    if (!IniGetU32(ini, kSec, "version", version) || version != kAutoSkillIniVersion) return false;
    AutoSkillSetDefaults(out);
    bool b = false;
    if (IniGetBool(ini, kSec, "enabled", b)) out.enabled = b ? 1u : 0u;
    const bool gotJ1en = IniGetBool(ini, kSec, "job1Enabled", b);
    if (gotJ1en) out.job1Enabled = b ? 1u : 0u;
    const bool gotJ2en = IniGetBool(ini, kSec, "job2Enabled", b);
    if (gotJ2en) out.job2Enabled = b ? 1u : 0u;
    uint32_t u = 0;
    if (IniGetU32(ini, kSec, "job1", u)) out.job1 = static_cast<int32_t>(u);
    if (IniGetU32(ini, kSec, "job2", u)) out.job2 = static_cast<int32_t>(u);
    std::string csv;
    if (IniGetString(ini, kSec, "job1Order", csv)) {
        ParseCsv(csv, out.job1Order, &out.job1Count, true, 0);
    }
    csv.clear();
    if (IniGetString(ini, kSec, "job2Order", csv)) {
        ParseCsv(csv, out.job2Order, &out.job2Count, true, 0);
    }
    csv.clear();
    if (IniGetString(ini, kSec, "job1Target", csv)) {
        ParseCsv(csv, out.job1Target, nullptr, false, kAutoSkillTargetMax);
    }
    csv.clear();
    if (IniGetString(ini, kSec, "job2Target", csv)) {
        ParseCsv(csv, out.job2Target, nullptr, false, kAutoSkillTargetMax);
    }
    if (!gotJ1en && !gotJ2en) {
        const bool on = out.enabled != 0;
        out.job1Enabled = 0;
        out.job2Enabled = 0;
        if (on) {
            if (AutoSkillIsExplorerJob1(out.job1) && out.job1Count > 0) out.job1Enabled = 1;
            if (AutoSkillIsExplorerJob2(out.job2) && out.job2Count > 0) out.job2Enabled = 1;
        }
    }
    AutoSkillNormalize(out);
    if (outWriteTick) IniGetU64(ini, kSec, "writeTickMs", *outWriteTick);
    return true;
}

bool WriteAutoSkillIni(const char* binDir, const AutoSkillConfig& cfg, uint64_t writeTickMs,
                       uint32_t lockTimeoutMs) {
    if (!binDir || !binDir[0]) return false;
    if (!EnsureStateDir(binDir)) return false;
    const std::string path = UserConfigIniPath(binDir);
    const std::string o1 = FormatCsv(cfg.job1Order, cfg.job1Count, true);
    const std::string o2 = FormatCsv(cfg.job2Order, cfg.job2Count, true);
    const std::string t1 = FormatCsv(cfg.job1Target, cfg.job1Count, false);
    const std::string t2 = FormatCsv(cfg.job2Target, cfg.job2Count, false);
    return UpdateIniFile(
        path.c_str(),
        [&](IniStore& ini) {
            IniSetU32(ini, "meta", "version", static_cast<uint32_t>(kUserConfigIniVersion));
            IniSetU32(ini, kSec, "version", kAutoSkillIniVersion);
            IniSetU64(ini, kSec, "writeTickMs", writeTickMs);
            IniSetBool(ini, kSec, "enabled", cfg.enabled != 0);
            IniSetBool(ini, kSec, "job1Enabled", cfg.job1Enabled != 0);
            IniSetBool(ini, kSec, "job2Enabled", cfg.job2Enabled != 0);
            IniSetU32(ini, kSec, "job1", static_cast<uint32_t>(cfg.job1 < 0 ? 0 : cfg.job1));
            IniSetU32(ini, kSec, "job2", static_cast<uint32_t>(cfg.job2 < 0 ? 0 : cfg.job2));
            IniSetString(ini, kSec, "job1Order", o1.c_str());
            IniSetString(ini, kSec, "job2Order", o2.c_str());
            IniSetString(ini, kSec, "job1Target", t1.c_str());
            IniSetString(ini, kSec, "job2Target", t2.c_str());
        },
        lockTimeoutMs);
}

}  // namespace

void AutoSkillSetDefaults(AutoSkillConfig& out) {
    out = AutoSkillConfig{};
    out.magic = kAutoSkillMagic;
    out.version = kAutoSkillVersion;
    out.enabled = kAutoSkillDefaultEnabled;
    out.job1Enabled = 0;
    out.job2Enabled = 0;
}

bool AutoSkillJob1Configured(const AutoSkillConfig& cfg) {
    return AutoSkillIsExplorerJob1(cfg.job1) && cfg.job1Count > 0;
}

bool AutoSkillJob2Configured(const AutoSkillConfig& cfg) {
    if (!AutoSkillIsExplorerJob2(cfg.job2) || cfg.job2Count == 0) return false;
    if (cfg.job1 && AutoSkillJobFamily(cfg.job1) != AutoSkillJobFamily(cfg.job2)) return false;
    return true;
}

void AutoSkillNormalize(AutoSkillConfig& cfg) {
    cfg.magic = kAutoSkillMagic;
    cfg.version = kAutoSkillVersion;
    CompactOrder(cfg.job1Order, cfg.job1Target, cfg.job1Count);
    CompactOrder(cfg.job2Order, cfg.job2Target, cfg.job2Count);
    if (!AutoSkillIsExplorerJob1(cfg.job1)) {
        cfg.job1 = 0;
        cfg.job1Count = 0;
        for (uint32_t i = 0; i < kAutoSkillOrderMax; ++i) {
            cfg.job1Order[i] = 0;
            cfg.job1Target[i] = 0;
        }
    }
    if (!AutoSkillIsExplorerJob2(cfg.job2) ||
        (cfg.job1 && AutoSkillJobFamily(cfg.job1) != AutoSkillJobFamily(cfg.job2))) {
        cfg.job2 = 0;
        cfg.job2Count = 0;
        for (uint32_t i = 0; i < kAutoSkillOrderMax; ++i) {
            cfg.job2Order[i] = 0;
            cfg.job2Target[i] = 0;
        }
    }
    if (!AutoSkillJob1Configured(cfg)) cfg.job1Enabled = 0;
    if (!AutoSkillJob2Configured(cfg)) cfg.job2Enabled = 0;
    cfg.enabled = cfg.enabled ? 1u : 0u;
}

bool AutoSkillReady(const AutoSkillConfig& cfg) {
    if (!cfg.enabled) return false;
    if (cfg.job1Enabled && AutoSkillJob1Configured(cfg)) return true;
    if (cfg.job2Enabled && AutoSkillJob2Configured(cfg)) return true;
    return false;
}

int AutoSkillJobFamily(int job) {
    if (job < 100 || job > 522) return 0;
    if (job >= 430 && job <= 434) return 0;
    return job / 100;
}

bool AutoSkillIsExplorerAdvancement(int job) { return AutoSkillJobFamily(job) != 0; }

bool AutoSkillSameJob2Branch(int charJob, int job2) {
    if (!AutoSkillIsExplorerJob2(job2)) return false;
    if (charJob == job2) return true;
    if (AutoSkillJobFamily(charJob) != AutoSkillJobFamily(job2)) return false;
    return (charJob / 10) == (job2 / 10) && charJob > job2;
}

int AutoSkillJob1OfJob2(int job2) {
    if (!AutoSkillIsExplorerJob2(job2)) return 0;
    return (job2 / 100) * 100;
}

bool AutoSkillIsExplorerJob1(int job) {
    for (int v : kJob1)
        if (v == job) return true;
    return false;
}

int AutoSkillBookJobOfSkill(int skillId) {
    if (skillId <= 0) return -1;
    if (skillId < 1000000) return 0;
    return skillId / 10000;
}

int AutoSkillJobLevel(int job) {
    if (job < 100) return 0;
    if (AutoSkillIsExplorerJob1(job)) return 1;
    if (AutoSkillIsExplorerJob2(job)) return 2;
    return 0;
}

bool AutoSkillIsExplorerJob2(int job) {
    switch (job) {
        case 110:
        case 120:
        case 130:
        case 210:
        case 220:
        case 230:
        case 310:
        case 320:
        case 410:
        case 420:
        case 510:
        case 520:
            return true;
        default:
            return false;
    }
}

void AutoSkillListJob1(int* out, uint32_t* count) {
    if (!out || !count) return;
    CopyJobList(kJob1, sizeof(kJob1) / sizeof(kJob1[0]), out, count);
}

void AutoSkillListJob2(int job1, int* out, uint32_t* count) {
    if (!out || !count) {
        return;
    }
    *count = 0;
    switch (job1) {
        case 100:
            CopyJobList(kJob2Warrior, sizeof(kJob2Warrior) / sizeof(kJob2Warrior[0]), out, count);
            break;
        case 200:
            CopyJobList(kJob2Mage, sizeof(kJob2Mage) / sizeof(kJob2Mage[0]), out, count);
            break;
        case 300:
            CopyJobList(kJob2Archer, sizeof(kJob2Archer) / sizeof(kJob2Archer[0]), out, count);
            break;
        case 400:
            CopyJobList(kJob2Thief, sizeof(kJob2Thief) / sizeof(kJob2Thief[0]), out, count);
            break;
        case 500:
            CopyJobList(kJob2Pirate, sizeof(kJob2Pirate) / sizeof(kJob2Pirate[0]), out, count);
            break;
        default: {
            if (job1 != 0) break;
            auto append = [&](const int* src, size_t n) {
                for (size_t i = 0; i < n && *count < 16; ++i) out[(*count)++] = src[i];
            };
            append(kJob2Warrior, sizeof(kJob2Warrior) / sizeof(kJob2Warrior[0]));
            append(kJob2Mage, sizeof(kJob2Mage) / sizeof(kJob2Mage[0]));
            append(kJob2Archer, sizeof(kJob2Archer) / sizeof(kJob2Archer[0]));
            append(kJob2Thief, sizeof(kJob2Thief) / sizeof(kJob2Thief[0]));
            append(kJob2Pirate, sizeof(kJob2Pirate) / sizeof(kJob2Pirate[0]));
            break;
        }
    }
}

const char* AutoSkillJobLabel(const char* binDir, int job) {
    if (job <= 0) return "选择职业";
    // 冒险家十五职名写死在 FallbackJobLabel。启动器 Combo 每帧/展开时不要为这几个字去解析 TSV。
    if (const char* fb = FallbackJobLabel(job)) return fb;
    if (!binDir || !binDir[0]) return "";
    const auto& pack = Pack(binDir);
    const auto it = pack.jobLabel.find(job);
    if (it != pack.jobLabel.end() && !it->second.empty()) return it->second.c_str();
    return "";
}

const char* AutoSkillSkillName(const char* binDir, int skillId) {
    (void)binDir;
    if (skillId <= 0) return "";
    if (const CatalogPack* pack = PeekCatalog()) {
        const auto it = pack->nameById.find(skillId);
        if (it != pack->nameById.end() && !it->second.empty()) return it->second.c_str();
    }
    if (const FallbackSkill* fb = FallbackSkillOf(skillId)) return fb->name;
    return "";
}

int AutoSkillMaxObserved(const char* binDir, int skillId) {
    (void)binDir;
    if (skillId <= 0) return 0;
    if (const CatalogPack* pack = PeekCatalog()) {
        const auto it = pack->maxObserved.find(skillId);
        if (it != pack->maxObserved.end() && it->second > 0) return it->second;
    }
    if (const FallbackSkill* fb = FallbackSkillOf(skillId)) return fb->maxLv;
    return 0;
}

void AutoSkillWarmCatalog(const char* binDir) {
    if (!binDir || !binDir[0]) return;
    (void)Pack(binDir);
}

void AutoSkillWarmCatalogAsync(const char* binDir) {
    if (!binDir || !binDir[0]) return;
    if (PeekCatalog()) return;
    static std::once_flag s_once;
    static std::string s_dir;
    s_dir = binDir;
    std::call_once(s_once, [] {
        HANDLE th = CreateThread(
            nullptr, 0,
            [](LPVOID) -> DWORD {
                AutoSkillWarmCatalog(s_dir.c_str());
                return 0;
            },
            nullptr, 0, nullptr);
        if (th) CloseHandle(th);
    });
}

bool AutoSkillFillDefaultOrder(const char* binDir, int job, int* ids, uint32_t* count,
                               int* targets) {
    if (!ids || !count) return false;
    for (uint32_t i = 0; i < kAutoSkillOrderMax; ++i) {
        ids[i] = 0;
        if (targets) targets[i] = 0;
    }
    *count = 0;
    if (job < 100) return false;
    // 启动器选职业那一帧禁止解析 TSV / Pack()：BIN 15:49:47 fill start 之后没有 fill done，
    // 就是 UI 线程卡在离线表或 user.ini 锁上。默认顺序写死冒险家书内技能。
    static const int k100[] = {1000000, 1000001, 1000002, 1001003, 1001004, 1001005};
    static const int k110[] = {1100000, 1100001, 1100002, 1100003, 1101004, 1101005, 1101006,
                               1101007};
    static const int k120[] = {1200000, 1200001, 1200002, 1200003, 1201004, 1201005, 1201006,
                               1201007};
    static const int k130[] = {1300000, 1300001, 1300002, 1300003, 1301004, 1301005, 1301006,
                               1301007};
    static const int k200[] = {2000000, 2000001, 2001002, 2001003, 2001004, 2001005};
    static const int k210[] = {2100000, 2101001, 2101002, 2101003, 2101004, 2101005};
    static const int k220[] = {2200000, 2201001, 2201002, 2201003, 2201004, 2201005};
    static const int k230[] = {2300000, 2301001, 2301002, 2301003, 2301004, 2301005};
    static const int k300[] = {3000000, 3000001, 3000002, 3001003, 3001004, 3001005};
    static const int k310[] = {3100000, 3100001, 3101002, 3101003, 3101004, 3101005};
    static const int k320[] = {3200000, 3200001, 3201002, 3201003, 3201004, 3201005};
    static const int k400[] = {4000000, 4000001, 4001002, 4001003, 4001334, 4001344};
    static const int k410[] = {4100000, 4100001, 4100002, 4101003, 4101004, 4101005};
    static const int k420[] = {4200000, 4200001, 4201002, 4201003, 4201004, 4201005};
    static const int k500[] = {5000000, 5001001, 5001002, 5001003, 5001005};
    static const int k510[] = {5100000, 5100001, 5101002, 5101003, 5101004, 5101005, 5101006,
                               5101007};
    static const int k520[] = {5200000, 5201001, 5201002, 5201003, 5201004, 5201005, 5201006};
    const int* src = nullptr;
    uint32_t n = 0;
    switch (job) {
        case 100: src = k100; n = sizeof(k100) / sizeof(k100[0]); break;
        case 110: src = k110; n = sizeof(k110) / sizeof(k110[0]); break;
        case 120: src = k120; n = sizeof(k120) / sizeof(k120[0]); break;
        case 130: src = k130; n = sizeof(k130) / sizeof(k130[0]); break;
        case 200: src = k200; n = sizeof(k200) / sizeof(k200[0]); break;
        case 210: src = k210; n = sizeof(k210) / sizeof(k210[0]); break;
        case 220: src = k220; n = sizeof(k220) / sizeof(k220[0]); break;
        case 230: src = k230; n = sizeof(k230) / sizeof(k230[0]); break;
        case 300: src = k300; n = sizeof(k300) / sizeof(k300[0]); break;
        case 310: src = k310; n = sizeof(k310) / sizeof(k310[0]); break;
        case 320: src = k320; n = sizeof(k320) / sizeof(k320[0]); break;
        case 400: src = k400; n = sizeof(k400) / sizeof(k400[0]); break;
        case 410: src = k410; n = sizeof(k410) / sizeof(k410[0]); break;
        case 420: src = k420; n = sizeof(k420) / sizeof(k420[0]); break;
        case 500: src = k500; n = sizeof(k500) / sizeof(k500[0]); break;
        case 510: src = k510; n = sizeof(k510) / sizeof(k510[0]); break;
        case 520: src = k520; n = sizeof(k520) / sizeof(k520[0]); break;
        default: break;
    }
    if (src && n) {
        for (uint32_t i = 0; i < n && i < kAutoSkillOrderMax; ++i) ids[(*count)++] = src[i];
    } else if (binDir && binDir[0]) {
        const auto& pack = Pack(binDir);
        for (const auto& s : pack.skills) {
            if (s.job != job) continue;
            if (pack.inBook.find(s.id) == pack.inBook.end()) continue;
            if (pack.invisible.find(s.id) != pack.invisible.end()) continue;
            if (pack.psd.find(s.id) != pack.psd.end()) continue;
            if (*count >= kAutoSkillOrderMax) break;
            ids[(*count)++] = s.id;
        }
    }
    if (targets) {
        for (uint32_t i = 0; i < *count; ++i) targets[i] = 0;
    }
    return *count > 0;
}

bool ReadAutoSkill(const char* binDir, AutoSkillConfig& out) {
    AutoSkillSetDefaults(out);
    uint64_t tick = 0;
    if (!ReadAutoSkillIni(binDir, out, &tick)) return false;
    out.writeTickMs = tick;
    AutoSkillNormalize(out);
    return true;
}

bool WriteAutoSkill(const char* binDir, const AutoSkillConfig& cfg) {
    return WriteAutoSkill(binDir, cfg, 8000);
}

bool WriteAutoSkill(const char* binDir, const AutoSkillConfig& cfg, uint32_t lockTimeoutMs) {
    AutoSkillConfig normalized = cfg;
    AutoSkillNormalize(normalized);
    const uint64_t tick = cfg.writeTickMs ? cfg.writeTickMs : GetTickCount64();
    normalized.writeTickMs = tick;
    return WriteAutoSkillIni(binDir, normalized, tick, lockTimeoutMs);
}

}  // namespace xcat
