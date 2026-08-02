#pragma once

#include <string>
#include <vector>

namespace xcat {

// 齐发勾选清单：固定 XCat_data\state\multiskill_select.tsv（与 OTA 备份范围一致）。
// 产品：经典版 TWMS · 不是枫星。实现面独立；契约路径对齐对照仓。

constexpr const char* kNormalAttackCode = "NormalAttack";
constexpr const char* kNormalAttackRelayCode = "NormalAttackRelay";
constexpr const char* kNormalAttackDisplayName = "普通攻击";

std::string MultiSkillSelectPath(const char* binDir);
std::string LearnedSkillsPath(const char* binDir);
std::string MultiSkillCastRequestPath(const char* binDir);

bool ReadMultiSkillSelect(const char* binDir, std::vector<std::string>& codes);
bool WriteMultiSkillSelect(const char* binDir, const std::vector<std::string>& codes);

// 面板「测试一波」：写空标记文件；payload 消费后删除。
bool WriteMultiSkillCastRequest(const char* binDir);
bool ConsumeMultiSkillCastRequest(const char* binDir);

// 已学技能缓存（payload 写、面板读）：code \t name \t level
struct LearnedSkillRow {
    char code[32]{};
    char name[128]{};
    int level = 0;
};
bool ReadLearnedSkillsTsv(const char* binDir, std::vector<LearnedSkillRow>& out);
bool WriteLearnedSkillsTsv(const char* binDir, const std::vector<LearnedSkillRow>& rows);

bool IsNumericSkillId(const char* code);
bool IsNormalAttackCode(const char* code);

}  // namespace xcat
