#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace xcat {

// 轻量 INI：section.key -> value。launcher 写、payload 低频读。
using IniStore = std::map<std::string, std::map<std::string, std::string>>;

constexpr int kUserConfigIniVersion = 1;

std::string UserConfigIniRelPath();
std::string UserConfigIniPath(const char* binDir);

bool LoadIniFile(const char* path, IniStore& out);
bool SaveIniFile(const char* path, const IniStore& store);

// 跨进程串行的读改写：持锁 → 载入（文件已存在但读失败则拒绝写，避免空表盖掉整份配置）→
// mutate → 原子落盘。所有 user.ini section 写入都应走这条路径。
bool UpdateIniFile(const char* path, const std::function<void(IniStore&)>& mutate);
// lockTimeoutMs=0：锁被占立刻失败，给 ImGui 线程用，禁止在绘制帧里干等。
bool UpdateIniFile(const char* path, const std::function<void(IniStore&)>& mutate,
                   uint32_t lockTimeoutMs);

bool IniGetString(const IniStore& ini, const char* section, const char* key, std::string& out);
bool IniGetU64(const IniStore& ini, const char* section, const char* key, uint64_t& out);
bool IniGetU32(const IniStore& ini, const char* section, const char* key, uint32_t& out);
// 带符号整数：给「可正可负的偏移量」用（如 F5 自定义站距的 Y）。
// 不要用 U32 加偏置去凑负数——user.ini 是用户会直接看/改的文件，偏置值读不懂也改不对。
bool IniGetI32(const IniStore& ini, const char* section, const char* key, int32_t& out);
bool IniGetBool(const IniStore& ini, const char* section, const char* key, bool& out);
bool IniGetFloat(const IniStore& ini, const char* section, const char* key, float& out);

void IniSetString(IniStore& ini, const char* section, const char* key, const char* value);
void IniSetU64(IniStore& ini, const char* section, const char* key, uint64_t value);
void IniSetU32(IniStore& ini, const char* section, const char* key, uint32_t value);
void IniSetI32(IniStore& ini, const char* section, const char* key, int32_t value);
void IniSetBool(IniStore& ini, const char* section, const char* key, bool value);
void IniSetFloat(IniStore& ini, const char* section, const char* key, float value);

// 删除 section 内单个 key（不存在则无操作）。
void IniEraseKey(IniStore& ini, const char* section, const char* key);

// 删除 section 内所有以 prefix 开头的 key。
// 写入约定：凡「prefix.N.field」可变/固定数组，Write 前必须先对本 prefix 调用本函数再按当前
// count/槽位数重写；启动迁移 UserConfigMigrateObsolete 会按 count 清 N>count 的历史孤儿。
void IniEraseKeysWithPrefix(IniStore& ini, const char* section, const char* prefix);

// 删除形如「prefix{N}.…」且 N>keepCount（1-based）的 key；返回删除条数。
size_t IniEraseIndexedKeysAbove(IniStore& ini, const char* section, const char* prefix,
                                uint32_t keepCount);

// 是否存在 N>keepCount（或 N==0）的「prefix{N}.…」key。
bool IniHasIndexedKeysAbove(const IniStore& ini, const char* section, const char* prefix,
                            uint32_t keepCount);

// 移除已废弃 section（如 [profile]），并按各数组 count/槽位上限清历史孤儿 key；有变更则写回。
bool UserConfigMigrateObsolete(const char* payloadBinDir);

}  // namespace xcat
