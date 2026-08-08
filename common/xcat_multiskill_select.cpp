#include "xcat_multiskill_select.h"

#include "process_util.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace xcat {
namespace {

bool EnsureStateDir(const char* binDir) {
    if (!binDir || !binDir[0]) return false;
    return CreateDirectoryUtf8(JoinBinPath(binDir, "state"));
}

bool ReadLinesFile(const char* path, std::vector<std::string>& lines) {
    lines.clear();
    if (!path || !path[0]) return false;
    FILE* f = nullptr;
    if (FopenUtf8(&f, path, L"rb") != 0 || !f) return false;
    char buf[256]{};
    while (fgets(buf, sizeof(buf), f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' ||
                              s.back() == '\t')) {
            s.pop_back();
        }
        size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i) s.erase(0, i);
        if (s.empty() || s[0] == '#') continue;
        lines.push_back(std::move(s));
    }
    fclose(f);
    return true;
}

bool WriteAtomicText(const std::string& path, const std::string& text) {
    if (path.empty()) return false;
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
        CreateDirectoryUtf8(path.substr(0, slash));
    }
    std::string tmp = path + ".tmp." + std::to_string(GetCurrentProcessId());
    FILE* f = nullptr;
    if (FopenUtf8(&f, tmp, L"wb") != 0 || !f) return false;
    if (!text.empty()) fputs(text.c_str(), f);
    const int flushRc = fflush(f);
    fclose(f);
    if (flushRc != 0) {
        DeleteFileUtf8(tmp);
        return false;
    }
    if (!MoveFileExUtf8(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileUtf8(tmp);
        return false;
    }
    return true;
}

}  // namespace

bool IsNumericSkillId(const char* code) {
    if (!code || !code[0]) return false;
    for (const char* p = code; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return code[0] != '0' || code[1] == 0;
}

bool IsNormalAttackCode(const char* code) {
    if (!code) return false;
    return strcmp(code, kNormalAttackCode) == 0 || strcmp(code, kNormalAttackRelayCode) == 0;
}

std::string MultiSkillSelectPath(const char* binDir) {
    return JoinBinPath(binDir, "state\\multiskill_select.tsv");
}

std::string LearnedSkillsPath(const char* binDir) {
    return JoinBinPath(binDir, "state\\learned_skills.tsv");
}

std::string MultiSkillCastRequestPath(const char* binDir) {
    return JoinBinPath(binDir, "state\\multiskill_cast_request");
}

bool ReadMultiSkillSelect(const char* binDir, std::vector<std::string>& codes) {
    codes.clear();
    if (!binDir || !binDir[0]) return false;
    const std::string path = MultiSkillSelectPath(binDir);
    std::vector<std::string> lines;
    if (!ReadLinesFile(path.c_str(), lines)) {
        return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    bool hasNa = false;
    for (std::string& s : lines) {
        if (IsNormalAttackCode(s.c_str())) {
            hasNa = true;
            continue;
        }
        bool dup = false;
        for (const std::string& old : codes) {
            if (old == s) {
                dup = true;
                break;
            }
        }
        if (!dup) codes.push_back(std::move(s));
    }
    // 与面板置顶一致：勾了普攻则始终排在串发队首（旧 tsv 常把 1000 写在前面）。
    if (hasNa) codes.insert(codes.begin(), kNormalAttackCode);
    return true;
}

bool WriteMultiSkillSelect(const char* binDir, const std::vector<std::string>& codes) {
    if (!EnsureStateDir(binDir)) return false;
    std::vector<std::string> out;
    out.reserve(codes.size());
    bool hasNa = false;
    for (const std::string& c : codes) {
        if (c.empty()) continue;
        if (IsNormalAttackCode(c.c_str())) {
            hasNa = true;
            continue;  // 稍后置顶写入，避免勾选追加序把普攻挤到蜗牛术后面
        }
        std::string id = c;
        bool dup = false;
        for (const std::string& old : out) {
            if (old == id) {
                dup = true;
                break;
            }
        }
        if (!dup) out.push_back(std::move(id));
    }
    if (hasNa) out.insert(out.begin(), kNormalAttackCode);
    std::string text = "# multi_skill select (one skill code per line; Classic TWMS)\n";
    for (const std::string& c : out) {
        text += c;
        text.push_back('\n');
    }
    return WriteAtomicText(MultiSkillSelectPath(binDir), text);
}

bool WriteMultiSkillCastRequest(const char* binDir) {
    if (!EnsureStateDir(binDir)) return false;
    return WriteAtomicText(MultiSkillCastRequestPath(binDir), "1\n");
}

bool ConsumeMultiSkillCastRequest(const char* binDir) {
    const std::string path = MultiSkillCastRequestPath(binDir);
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    DeleteFileUtf8(path);
    return true;
}

bool ReadLearnedSkillsTsv(const char* binDir, std::vector<LearnedSkillRow>& out) {
    out.clear();
    std::vector<std::string> lines;
    if (!ReadLinesFile(LearnedSkillsPath(binDir).c_str(), lines)) return false;
    for (const std::string& line : lines) {
        LearnedSkillRow row{};
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) {
            if (!IsNumericSkillId(line.c_str()) && !IsNormalAttackCode(line.c_str())) continue;
            strncpy_s(row.code, line.c_str(), _TRUNCATE);
            out.push_back(row);
            continue;
        }
        const std::string code = line.substr(0, t1);
        size_t t2 = line.find('\t', t1 + 1);
        std::string name;
        int level = 0;
        if (t2 == std::string::npos) {
            name = line.substr(t1 + 1);
        } else {
            name = line.substr(t1 + 1, t2 - t1 - 1);
            level = atoi(line.c_str() + t2 + 1);
        }
        strncpy_s(row.code, code.c_str(), _TRUNCATE);
        strncpy_s(row.name, name.c_str(), _TRUNCATE);
        row.level = level;
        out.push_back(row);
    }
    return true;
}

bool WriteLearnedSkillsTsv(const char* binDir, const std::vector<LearnedSkillRow>& rows) {
    if (!EnsureStateDir(binDir)) return false;
    std::string text = "# code\tname\tlevel\n";
    for (const LearnedSkillRow& r : rows) {
        if (!r.code[0]) continue;
        text += r.code;
        text.push_back('\t');
        text += r.name[0] ? r.name : r.code;
        text.push_back('\t');
        text += std::to_string(r.level);
        text.push_back('\n');
    }
    return WriteAtomicText(LearnedSkillsPath(binDir), text);
}

}  // namespace xcat
