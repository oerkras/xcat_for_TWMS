#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace xcat {

// 离线启发式分类（skill_catalog_full.tsv · 对照枫星 GUI 语义；非官方 SkillType 枚举）
// 0=被动/未知  1=辅助(BUFF/治疗/祝福等)  2=攻击
constexpr int kSkillCatalogTypeUnknown = 0;
constexpr int kSkillCatalogTypeSupport = 1;
constexpr int kSkillCatalogTypeAttack  = 2;

// 经典版离线技能名 + 类型：
//   dataservice/skill_names.tsv（code / Name）
//   dataservice/skill_catalog_full.tsv（code / name / job / type / …）
struct SkillNamesPack {
    bool loaded = false;
    bool typesLoaded = false;
    // 键为去前导零的数字码（"1000"）；重名保留首次
    std::unordered_map<std::string, std::string> nameByCode;
    std::unordered_map<std::string, int> typeByCode;
};

bool LoadSkillNamesPack(const char* payloadBinDir, SkillNamesPack& out);
const SkillNamesPack& GetSharedSkillNames(const char* payloadBinDir);

// code 可为 "1000" / "0001000"；未命中返回 ""
const char* SkillNameLookup(const SkillNamesPack& pack, const char* code);
const char* SkillNameLookupById(const SkillNamesPack& pack, int skillId);

// 未命中返回 -1；命中返回 0/1/2
int SkillCatalogTypeLookup(const SkillNamesPack& pack, const char* code);

// BUFF 列表候选：辅助 / 表外未知；攻击与被动默认否。
// keepIfActiveOrEnabled：在身或已开槽时强制保留。
bool SkillLooksLikeBuffCandidate(const SkillNamesPack& pack, const char* code,
                                 bool keepIfActiveOrEnabled);

// 技能多发列表候选：仅攻击(type=2)；辅助/被动默认否。
// keepIfSelected：已勾选时强制保留（便于取消勾选）。
bool SkillLooksLikeAttackCandidate(const SkillNamesPack& pack, const char* code,
                                   bool keepIfSelected);

}  // namespace xcat
