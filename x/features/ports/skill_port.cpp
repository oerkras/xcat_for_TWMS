// skill_port �?Classic TWMS skill presence / learn / cast for buffs.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "skill_port.h"

#include "world_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/main_thread_pump.h"

#include "xcat_skill_names.h"

#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::skill {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr uint32_t kRvaFindObjectsOfTypeAll = 0x4E3FA20;  // remapped 2026-08-03
constexpr uint32_t kRvaCompGetGo = 0x4E47E00;  // remapped 2026-08-03
constexpr uint32_t kRvaObjGetName = 0x4E54D60;  // remapped 2026-08-03
constexpr uint32_t kRvaGetSkillLevel = 0x1063850;  // remapped 2026-08-03
// 公开总入口 DoActiveSkill(int skillId, uint scanCode=0) —— 经典版对标枫星 UseOnClientImmediate。
constexpr uint32_t kRvaDoActiveSkill = 0x1064E60;  // remapped 2026-08-03
constexpr uint32_t kRvaDoActiveSkillPrepare = 0x10A72A0;  // remapped 2026-08-03
constexpr uint32_t kRvaSendSkillUseRequest = 0x10BBDF0;  // remapped 2026-08-03
// SkillInfo.GetSkill(int) —— TW idx4 N/1 紧随 GetSkillRoot；短函数 dict 查找。
constexpr uint32_t kRvaSkillInfoGetSkill = 0x142A590;  // remapped 2026-08-03
// SecondaryStat.GetRemainTime(int nSkillID, int tCur) —— 返回剩余（与 tXxx_ 同时钟）。
constexpr uint32_t kRvaSecondaryStatGetRemainTime = 0xD61DF0;  // remapped 2026-08-03

// 方法哈希（dump 名；RVA 漂时优先）
constexpr char kHashGetSkillLevel[] =
    "e5d854e8495ca42d2032aaa642b8ddaa4a66e88b261284548997340c40e76c3";
constexpr char kHashDoActiveSkill[] =
    "c4f5e18dc5e7302ea23d1490a19438f79b84f05f69084d05a430000fd6d9e61";
constexpr char kHashDoActiveSkillPrepare[] =
    "a9204e71caccc9e2150afb63f98a01b6f354d874bf830e9569420b77ede53d4";
constexpr char kHashSkillInfoGetSkill[] =
    "de1e41ea2ec63589ab84c6fcbe9c5f37f84d8fa6c4bd849aedf42eab3188abd";
constexpr char kHashGetRemainTime[] =
    "e452dc76015d1c5631801d953e30c9e61407f49176a90ace243654f6b5073cb";
// UserLocal：il2cpp_shape::ResolveUserLocalKlass（hash ac2e48cc… + Teleport@0x3C8）
// SkillInfo：4×Dict + List 形；旧 b4dbdfd3… 已失效。
constexpr char kSkillInfoClass[] =
    "cd80f688c990f0dd0aafd2b78602618c46a424e5b4c34d18172867f24c782ec";
// WM+0xF0 SecondaryStat（字段全表）；勿与 WM+0xB8 嵌套 struct 混淆。
constexpr char kSecondaryStatClass[] =
    "e9c12ac2dac840eb205b1c8885835869a346ee08ba105bc3eeb41dcbca8e9d1";

constexpr size_t kOffWmMyUser = 0x28;
constexpr size_t kOffWmCharacterData = 0xE0;
constexpr size_t kOffWmSecondaryStat = 0xF0;
constexpr size_t kOffAffectedList = 0x330;
constexpr size_t kOffSkillRecord = 0x50;
constexpr size_t kOffSkillRecordEx = 0x58;
constexpr size_t kOffSkillCooltime = 0x68;
constexpr size_t kOffSkillInfoDict = 0x10;  // SkillInfo._dictionarySkill
constexpr size_t kOffSkillId = 0x10;
constexpr size_t kOffSkillName = 0x18;
constexpr size_t kOffAffSkillId = 0x10;
constexpr size_t kOffAffStartTime = 0x14;
constexpr size_t kOffCachedPtr = 0x10;
constexpr size_t kOffVisPos = 0x64;
constexpr size_t kOffLogicalPos = 0x240;

// Dictionary IL2CPP：buckets@0x10 / entries@0x18 / count@0x20 / freeCount@0x28 / version@0x2C
// （ForEach* 以 entries 扫描为主；DictIntIntCount 的 free 仅作 hint）
constexpr size_t kOffDictEntries = 0x18;
constexpr size_t kOffDictCount = 0x20;
constexpr size_t kOffDictFreeCount = 0x28;
// int,int 紧凑 0x10；部分 IL2CPP 对齐成 0x18（value@0x10）。
constexpr size_t kEntrySizeIntIntTight = 0x10;
constexpr size_t kEntrySizeIntPtr = 0x18;

constexpr DWORD kRebindMs = 3000;
constexpr DWORD kJobWaitMs = 2000;
constexpr float kMinPosAbs = 1.0f;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);
using FnGetSkillLevel = int (*)(void* self, int skillId, const void* methodInfo);
using FnGetSkill = void* (*)(void* self, int skillId, const void* methodInfo);
using FnDoActiveSkill = bool (*)(void* self, int skillId, uint32_t scanCode, const void* methodInfo);
using FnPrepare = bool (*)(void* self, void* skill, int level, uint32_t scanCode,
                           const void* methodInfo);
using FnGetRemainTime = int (*)(void* self, int skillId, int tCur, const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
FnClassStaticData gClassStaticData = nullptr;
FnClassParent gClassParent = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;
FnGetSkillLevel gGetSkillLevel = nullptr;
FnGetSkill gGetSkill = nullptr;
FnDoActiveSkill gDoActiveSkill = nullptr;
FnPrepare gPrepare = nullptr;
FnGetRemainTime gGetRemainTime = nullptr;

void* gLuType = nullptr;
void* gLocalUser = nullptr;
void* gSkillInfoKlass = nullptr;
void* gSkillInfo = nullptr;
void* gLocalUserKlass = nullptr;
void* gSecondaryStatKlass = nullptr;
MethodInfoHead* gMiGetSkillLevel = nullptr;
MethodInfoHead* gMiGetSkill = nullptr;
MethodInfoHead* gMiDoActiveSkill = nullptr;
MethodInfoHead* gMiPrepare = nullptr;
MethodInfoHead* gMiGetRemainTime = nullptr;

DWORD gLastLuRebind = 0;
DWORD gLastSiRebind = 0;
DWORD gLastDictDiagMs = 0;
DWORD gLastAffDiagMs = 0;
DWORD gLastRemainDiagMs = 0;
std::atomic<bool> gBound{false};

struct CastJob {
    int skillId = 0;
    bool ok = false;
    bool notReady = false;
    char reason[48]{};
};

template <typename T>
T AtRva(uint32_t rva) {
    return reinterpret_cast<T>(reinterpret_cast<uint8_t*>(gGA) + rva);
}

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint16_t ReadU16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

float ReadF32(void* obj, size_t off) {
    if (!obj) return 0.f;
    __try {
        return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.f;
    }
}

void* FindClass(const char* name) {
    return x::runtime::il2cpp::FindClass("", name);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

void* KlassStaticFields(void* klass) {
    if (!klass || !gClassStaticData) return nullptr;
    __try {
        return gClassStaticData(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* TryLazyValue(void* lazy) {
    if (!LooksLikeHeapPtr(lazy)) return nullptr;
    // Lazy<T>：value 常见 @+0x08/+0x10/+0x18/+0x20/+0x28（对齐 drop_pool）。
    const size_t offs[] = {0x08, 0x10, 0x18, 0x20, 0x28};
    for (size_t off : offs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

bool ObjKlassIs(void* obj, void* expectKlass) {
    if (!obj || !expectKlass || !LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) == expectKlass;
}

bool GetGoName(void* comp, char* out, int outSz) {
    if (!comp || !out || outSz <= 0 || !gCompGo || !gObjName) return false;
    out[0] = 0;
    __try {
        void* go = gCompGo(comp, nullptr);
        if (!go) return false;
        void* nameObj = gObjName(go, nullptr);
        if (!nameObj) return false;
        // System.String: length @+0x10, chars @+0x14
        const int len = ReadI32(nameObj, 0x10);
        if (len <= 0 || len > 120) return false;
        const char16_t* chars =
            reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(nameObj) + 0x14);
        int n = 0;
        for (int i = 0; i < len && n + 1 < outSz; ++i) {
            const char16_t c = chars[i];
            if (c < 128) out[n++] = static_cast<char>(c);
            else out[n++] = '?';
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsMyUserGo(void* user) {
    char name[64]{};
    return GetGoName(user, name, sizeof(name)) && _stricmp(name, "MyUser") == 0;
}

bool ReadIl2CppString(void* strObj, char* out, int outSz) {
    if (!strObj || !out || outSz <= 0) return false;
    out[0] = 0;
    __try {
        const int len = ReadI32(strObj, 0x10);
        if (len <= 0 || len > 200) return false;
        const char16_t* chars =
            reinterpret_cast<const char16_t*>(reinterpret_cast<uint8_t*>(strObj) + 0x14);
        int n = 0;
        for (int i = 0; i < len && n + 1 < outSz; ++i) {
            const char16_t c = chars[i];
            if (c < 128)
                out[n++] = static_cast<char>(c);
            else if (c < 0x800 && n + 2 < outSz) {
                out[n++] = static_cast<char>(0xC0 | (c >> 6));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            } else if (n + 3 < outSz) {
                out[n++] = static_cast<char>(0xE0 | (c >> 12));
                out[n++] = static_cast<char>(0x80 | ((c >> 6) & 0x3F));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            }
        }
        out[n] = 0;
        return n > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int ListSize(void* list) {
    if (!list) return 0;
    return ReadI32(list, 0x18);
}

void* ListItems(void* list) { return ReadPtr(list, 0x10); }

void* ArrayAtPtr(void* arr, int i) {
    if (!arr || i < 0) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                         static_cast<size_t>(i) * sizeof(void*));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ResolveApi() {
    if (gGA && gFindAll && gGetSkillLevel && gPrepare && gGetSkill) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gCompGo = e.compGo;
    gObjName = e.objName;
    gClassGetMethods = e.classGetMethods;
    gClassStaticData = e.classStaticData;
    gClassParent = e.classParent;
    gGetSkillLevel = x::runtime::il2cpp::AtRva<FnGetSkillLevel>(kRvaGetSkillLevel);
    gDoActiveSkill = x::runtime::il2cpp::AtRva<FnDoActiveSkill>(kRvaDoActiveSkill);
    gPrepare = x::runtime::il2cpp::AtRva<FnPrepare>(kRvaDoActiveSkillPrepare);
    gGetSkill = x::runtime::il2cpp::AtRva<FnGetSkill>(kRvaSkillInfoGetSkill);
    gGetRemainTime =
        x::runtime::il2cpp::AtRva<FnGetRemainTime>(kRvaSecondaryStatGetRemainTime);
    return gFindAll && gGetSkillLevel && gDoActiveSkill && gPrepare && gGetSkill;
}

bool LocalUserStillAlive() {
    if (!gLocalUser) return false;
    __try {
        if (!*reinterpret_cast<void**>(gLocalUser)) return false;
        const intptr_t cached =
            *reinterpret_cast<intptr_t*>(reinterpret_cast<uint8_t*>(gLocalUser) + kOffCachedPtr);
        if (!cached) return false;
        // 换图后旧 GO 仍可能叫 MyUser；以 WM.MyUser 为准强制失效。
        if (world::EnsureBound()) {
            void* wm = world::PeekWorldManager();
            void* mu = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
            if (LooksLikeHeapPtr(mu) && mu != gLocalUser) return false;
        }
        char name[64]{};
        if (!GetGoName(gLocalUser, name, sizeof(name))) return false;
        if (_stricmp(name, "MyUser") != 0) return false;
        const float vx = ReadF32(gLocalUser, kOffVisPos);
        const float vy = ReadF32(gLocalUser, kOffVisPos + 4);
        const float lx = ReadF32(gLocalUser, kOffLogicalPos);
        const float ly = ReadF32(gLocalUser, kOffLogicalPos + 4);
        if (fabsf(vx) < kMinPosAbs && fabsf(vy) < kMinPosAbs && fabsf(lx) < kMinPosAbs &&
            fabsf(ly) < kMinPosAbs)
            return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryResolveLocalUser(DWORD now) {
    if (LocalUserStillAlive()) return true;

    bool forceRebind = false;
    if (gLocalUser) {
        void* wm = world::PeekWorldManager();
        void* mu = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
        if (LooksLikeHeapPtr(mu) && mu != gLocalUser) forceRebind = true;
    }
    gLocalUser = nullptr;
    if (!forceRebind && gLastLuRebind && now - gLastLuRebind < kRebindMs) return false;
    gLastLuRebind = now;

    // 优先 WM.MyUser（与 drop/combat/travel 同真源）。
    if (world::EnsureBound()) {
        void* wm = world::GetWorldManager();
        void* mu = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
        if (LooksLikeHeapPtr(mu) && IsMyUserGo(mu)) {
            gLocalUser = mu;
            runtime::LogI("SkillPort", "LocalUser ACCEPT wm.MyUser=%p", gLocalUser);
            return true;
        }
    }

    if (!gFindAll) return false;
    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    if (!gLuType) return false;
    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(gFindAll, gLuType, 2000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!arr) return false;
    const int n = ReadI32(arr, 0x18);
    void* best = nullptr;
    for (int i = 0; i < n && i < 64; ++i) {
        void* cand = ArrayAtPtr(arr, i);
        if (!LooksLikeHeapPtr(cand) || !IsMyUserGo(cand)) continue;
        best = cand;
        break;
    }
    gLocalUser = best;
    if (gLocalUser) runtime::LogI("SkillPort", "LocalUser ACCEPT FindAll=%p", gLocalUser);
    return gLocalUser != nullptr;
}

bool LooksLikeSkillInfo(void* cand) {
    if (!LooksLikeHeapPtr(cand)) return false;
    if (gSkillInfoKlass && !ObjKlassIs(cand, gSkillInfoKlass)) return false;
    // SkillInfo._dictionarySkill @+0x10
    return LooksLikeHeapPtr(ReadPtr(cand, kOffSkillInfoDict));
}

void* ResolveSkillInfoSingleton(DWORD now) {
    if (LooksLikeSkillInfo(gSkillInfo)) return gSkillInfo;
    if (now - gLastSiRebind < kRebindMs) return gSkillInfo;
    gLastSiRebind = now;
    gSkillInfo = nullptr;
    if (!gSkillInfoKlass) gSkillInfoKlass = FindClass(kSkillInfoClass);
    if (!gSkillInfoKlass) return nullptr;
    // No RuntimeClassInit on caller thread — EnsureBound/resolve often run on workers
    // (buffs/multi_skill). Nested managed_main::Call from CastJobFn (already on main)
    // would also deadlock the pump. Read statics only; game cctor owns init.

    void* staticsKlass = gSkillInfoKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gSkillInfoKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) staticsKlass = parent;
    }

    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gSkillInfoKlass);

    void* best = nullptr;
    if (statics) {
        // Singleton<T>.Lazy 通常在 parent statics +0；扫宽对齐 drop_pool。
        for (size_t s = 0; s <= 0x40; s += sizeof(void*)) {
            void* lazy = ReadPtr(statics, s);
            void* cand = TryLazyValue(lazy);
            if (!cand) cand = lazy;
            if (LooksLikeSkillInfo(cand)) {
                best = cand;
                break;
            }
        }
    }

    // 再扫 SkillInfo 自身 statics（部分构建 Lazy 不在 parent）。
    if (!best) {
        void* own = KlassStaticFields(gSkillInfoKlass);
        if (own && own != statics) {
            for (size_t s = 0; s <= 0x40; s += sizeof(void*)) {
                void* lazy = ReadPtr(own, s);
                void* cand = TryLazyValue(lazy);
                if (!cand) cand = lazy;
                if (LooksLikeSkillInfo(cand)) {
                    best = cand;
                    break;
                }
            }
        }
    }

    if (best) {
        gSkillInfo = best;
        runtime::LogI("SkillPort", "SkillInfo ACCEPT si=%p", gSkillInfo);
        return gSkillInfo;
    }
    runtime::LogWThrottled(31, 15000, "SkillPort", "SkillInfo resolve miss statics=%p klass=%p",
                           statics, gSkillInfoKlass);
    return nullptr;
}

void* CharacterData() {
    void* wm = world::GetWorldManager();
    if (!wm) return nullptr;
    return ReadPtr(wm, kOffWmCharacterData);
}

int DictIntIntCount(void* dict) {
    if (!dict) return 0;
    const int count = ReadI32(dict, kOffDictCount);
    const int freeCount = ReadI32(dict, kOffDictFreeCount);
    int n = count - freeCount;
    if (n < 0) n = count;
    return n;
}

bool LooksLikePlayerSkillId(int id) {
    // 初心者 1000+；职业技通常 7~8 位。排除明显脏键。
    return id >= 1000 && id <= 99999999;
}

bool LooksLikeSkillLevel(int lv) { return lv > 0 && lv <= 60; }

int ScoreIntIntLayout(void* entries, int len, size_t stride, size_t valOff) {
    int score = 0;
    if (!entries || len <= 0 || stride < 16) return 0;
    const int n = len < 4096 ? len : 4096;
    for (int i = 0; i < n; ++i) {
        uint8_t* e = reinterpret_cast<uint8_t*>(entries) + 0x20 + static_cast<size_t>(i) * stride;
        __try {
            const int hash = *reinterpret_cast<int*>(e + 0);
            if (hash < 0) continue;
            const int key = *reinterpret_cast<int*>(e + 8);
            const int val = *reinterpret_cast<int*>(e + valOff);
            if (LooksLikePlayerSkillId(key) && LooksLikeSkillLevel(val)) ++score;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return score;
}

// Walk Dictionary<int,int>；自动在 0x10(value@12) / 0x18(value@16) 间择优。
template <typename Fn>
void ForEachDictIntInt(void* dict, Fn&& fn) {
    if (!dict) return;
    void* entries = ReadPtr(dict, kOffDictEntries);
    if (!entries) return;
    const int len = ReadI32(entries, 0x18);
    if (len <= 0) return;

    const int scoreTight = ScoreIntIntLayout(entries, len, kEntrySizeIntIntTight, 12);
    const int scoreAlign = ScoreIntIntLayout(entries, len, kEntrySizeIntPtr, 16);
    const size_t stride = (scoreAlign > scoreTight) ? kEntrySizeIntPtr : kEntrySizeIntIntTight;
    const size_t valOff = (stride == kEntrySizeIntPtr) ? 16u : 12u;

    for (int i = 0; i < len && i < 4096; ++i) {
        uint8_t* e = reinterpret_cast<uint8_t*>(entries) + 0x20 + static_cast<size_t>(i) * stride;
        __try {
            const int hash = *reinterpret_cast<int*>(e + 0);
            if (hash < 0) continue;
            const int key = *reinterpret_cast<int*>(e + 8);
            const int val = *reinterpret_cast<int*>(e + valOff);
            if (key <= 0) continue;
            fn(key, val);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !gClassGetMethods || !gGA) return nullptr;
    const uintptr_t want = reinterpret_cast<uintptr_t>(AtRva<void*>(rva));
    // 本类 + 父类链（DoActive/Prepare 可能声明在 UserLocal 基类上）。
    void* cur = klass;
    for (int depth = 0; cur && depth < 16; ++depth) {
        void* iter = nullptr;
        for (;;) {
            void* miRaw = nullptr;
            __try {
                miRaw = gClassGetMethods(cur, &iter);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                break;
            }
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            if (reinterpret_cast<uintptr_t>(mi->methodPointer) == want) return mi;
        }
        if (!gClassParent) break;
        void* parent = nullptr;
        __try {
            parent = gClassParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            break;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* FindMethodByName(void* klass, const char* name, int argc) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    MethodInfoHead* mi = nullptr;
    if (e.classGetMethodFromName) {
        __try {
            mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethodFromName(klass, name, argc));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            mi = nullptr;
        }
    }
    if (mi && mi->methodPointer) return mi;
    if (!e.classGetMethods || !e.methodGetName) return nullptr;
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        void* iter = nullptr;
        __try {
            for (;;) {
                void* raw = e.classGetMethods(cur, &iter);
                if (!raw) break;
                const char* nm = e.methodGetName(raw);
                if (nm && strcmp(nm, name) == 0) {
                    mi = reinterpret_cast<MethodInfoHead*>(raw);
                    if (mi && mi->methodPointer) return mi;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (!e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (!parent || parent == cur) break;
        cur = parent;
    }
    return nullptr;
}

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash) {
    if (plain) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plain, shape.arity)) return mi;
    }
    if (hash) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hash, shape.arity)) return mi;
    }
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("Skill", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plain ? plain : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, rva);
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

void EnsureMethodInfos() {
    if (!gLocalUserKlass) gLocalUserKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    if (!gSkillInfoKlass) gSkillInfoKlass = FindClass(kSkillInfoClass);
    if (!gSecondaryStatKlass) gSecondaryStatKlass = FindClass(kSecondaryStatClass);
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (gLocalUserKlass) {
        if (!gMiGetSkillLevel) {
            // int(int) 不唯一 → 哈希主；kind 验形。
            constexpr MethodShape kLv{1, TypeKind::I32, true, true, {TypeKind::I32}};
            gMiGetSkillLevel = ResolveMi(gLocalUserKlass, kRvaGetSkillLevel, kLv, "GetSkillLevel",
                                         kHashGetSkillLevel);
        }
        if (!gMiDoActiveSkill) {
            // bool(int,uint) 唯一。
            constexpr MethodShape kDo{
                2, TypeKind::Bool, true, true, {TypeKind::I32, TypeKind::U32}};
            gMiDoActiveSkill = ResolveMi(gLocalUserKlass, kRvaDoActiveSkill, kDo, "DoActiveSkill",
                                         kHashDoActiveSkill);
        }
        if (!gMiPrepare) {
            // bool(SkillEntry?,int,uint) — 哈希主。
            constexpr MethodShape kPrep{
                3, TypeKind::Bool, true, true, {TypeKind::Ptr, TypeKind::I32, TypeKind::U32}};
            gMiPrepare = ResolveMi(gLocalUserKlass, kRvaDoActiveSkillPrepare, kPrep,
                                   "DoActiveSkillPrepare", kHashDoActiveSkillPrepare);
        }
    }
    if (gSkillInfoKlass && !gMiGetSkill) {
        constexpr MethodShape kGet{1, TypeKind::Ptr, true, false, {TypeKind::I32}};
        gMiGetSkill = ResolveMi(gSkillInfoKlass, kRvaSkillInfoGetSkill, kGet, "GetSkill",
                                kHashSkillInfoGetSkill);
    }
    if (gSecondaryStatKlass && !gMiGetRemainTime) {
        // int(int,int) 唯一。
        constexpr MethodShape kRem{2, TypeKind::I32, true, false, {TypeKind::I32, TypeKind::I32}};
        gMiGetRemainTime = ResolveMi(gSecondaryStatKlass, kRvaSecondaryStatGetRemainTime, kRem,
                                     "GetRemainTime", kHashGetRemainTime);
    }
    // 函数指针与 MI 对齐（防只更 RVA 常量）。
    if (gMiGetSkillLevel) gGetSkillLevel = FnFromMi<FnGetSkillLevel>(gMiGetSkillLevel, kRvaGetSkillLevel);
    if (gMiDoActiveSkill) gDoActiveSkill = FnFromMi<FnDoActiveSkill>(gMiDoActiveSkill, kRvaDoActiveSkill);
    if (gMiPrepare) gPrepare = FnFromMi<FnPrepare>(gMiPrepare, kRvaDoActiveSkillPrepare);
    if (gMiGetSkill) gGetSkill = FnFromMi<FnGetSkill>(gMiGetSkill, kRvaSkillInfoGetSkill);
    if (gMiGetRemainTime)
        gGetRemainTime = FnFromMi<FnGetRemainTime>(gMiGetRemainTime, kRvaSecondaryStatGetRemainTime);
}

void SetJobReason(CastJob* job, const char* why) {
    if (!job) return;
    strncpy_s(job->reason, why ? why : "fail", _TRUNCATE);
}

// tXxx_ = 绝对到期时刻（与 invuln 写入 GetTickCount()+时长 同语义），不是剩余秒。
// 旧逻辑把脏扫到的小数当成秒 → UI 出现 167s/146s（截图：游戏图标 1/22）。
float RemainFromSecondaryStat(int skillId) {
    void* wm = world::GetWorldManager();
    if (!wm || skillId <= 0) return 0.f;
    void* ss = ReadPtr(wm, kOffWmSecondaryStat);
    if (!LooksLikeHeapPtr(ss)) return 0.f;
    if (!gSecondaryStatKlass) gSecondaryStatKlass = FindClass(kSecondaryStatClass);
    if (gSecondaryStatKlass && !ObjKlassIs(ss, gSecondaryStatKlass)) return 0.f;

    constexpr float kMaxBuffRemainSec = 600.f;
    constexpr int kMaxRemainMs = 600000;
    const int tCur = static_cast<int>(GetTickCount());

    auto remFromRaw = [&](int raw) -> float {
        if (raw <= 0) return 0.f;
        // 优先：绝对到期 − 当前（ms）
        const int remMs = raw - tCur;
        if (remMs > 0 && remMs <= kMaxRemainMs) return static_cast<float>(remMs) / 1000.f;
        // GetRemainTime 偶发直接返回剩余 ms
        if (raw > static_cast<int>(kMaxBuffRemainSec) && raw <= kMaxRemainMs)
            return static_cast<float>(raw) / 1000.f;
        // 禁止把 raw∈(0,600] 当剩余秒（脏命中根因）
        return 0.f;
    };

    float best = 0.f;
    float fromApi = 0.f;
    if (gGetRemainTime) {
        EnsureMethodInfos();
        int apiRaw = 0;
        __try {
            apiRaw = gGetRemainTime(ss, skillId, tCur, gMiGetRemainTime);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            apiRaw = 0;
        }
        fromApi = remFromRaw(apiRaw);
        // API 已返回「剩余 ms/秒」：apiRaw 本身可能已是 remMs（非绝对时间）
        if (fromApi <= 0.f && apiRaw > 0 && apiRaw <= kMaxRemainMs) {
            if (apiRaw <= static_cast<int>(kMaxBuffRemainSec))
                fromApi = static_cast<float>(apiRaw);  // 剩余秒
            else
                fromApi = static_cast<float>(apiRaw) / 1000.f;
        }
        if (fromApi > best) best = fromApi;
    }

    float fromScan = 0.f;
    int hitT = 0;
    int hitOff = 0;
    __try {
        for (size_t off = 0x10; off + 8 < 0x600; off += 4) {
            const size_t mod = off % 16u;
            if (mod != 0x08 && mod != 0x04) continue;
            const int r = ReadI32(ss, off);
            if (r != skillId) continue;
            const int t = ReadI32(ss, off + 4);
            const float sec = remFromRaw(t);
            if (sec <= 0.f) continue;
            if (sec > fromScan) {
                fromScan = sec;
                hitT = t;
                hitOff = static_cast<int>(off);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (fromScan > best) best = fromScan;

    const DWORD now = GetTickCount();
    if (best > 0.01f && now - gLastRemainDiagMs >= 8000) {
        gLastRemainDiagMs = now;
        runtime::LogW("SkillPort",
                      "remain id=%d best=%.1f api=%.1f scan=%.1f t=%d off=0x%X tCur=%d", skillId,
                      best, fromApi, fromScan, hitT, hitOff, tCur);
    }
    return best;
}

float CooltimeRemainSec(int skillId) {
    void* cd = CharacterData();
    if (!cd || skillId <= 0) return 0.f;
    void* dict = ReadPtr(cd, kOffSkillCooltime);
    if (!dict) return 0.f;
    void* entries = ReadPtr(dict, kOffDictEntries);
    if (!entries) return 0.f;
    const int len = ReadI32(entries, 0x18);
    if (len <= 0) return 0.f;
    // Dictionary<int,ushort>：先按「合理 CD 值」给布局打分，避免读到错位 ushort。
    struct Cand {
        size_t stride;
        size_t valOff;
        bool asU16;
    };
    const Cand cands[] = {
        {kEntrySizeIntIntTight, 12u, true},
        {kEntrySizeIntPtr, 16u, true},
        {kEntrySizeIntIntTight, 12u, false},
        {kEntrySizeIntPtr, 16u, false},
    };
    auto readVal = [&](const Cand& c, uint8_t* e) -> int {
        if (c.asU16) return static_cast<int>(*reinterpret_cast<uint16_t*>(e + c.valOff));
        return *reinterpret_cast<int*>(e + c.valOff);
    };
    auto saneCd = [](int v) -> bool { return v > 0 && v <= 600; };  // 秒；技能 CD 很少 >10 分钟
    int bestScore = -1;
    size_t bestIdx = 0;
    for (size_t ci = 0; ci < sizeof(cands) / sizeof(cands[0]); ++ci) {
        const Cand& c = cands[ci];
        int score = 0;
        for (int i = 0; i < len && i < 256; ++i) {
            uint8_t* e =
                reinterpret_cast<uint8_t*>(entries) + 0x20 + static_cast<size_t>(i) * c.stride;
            __try {
                if (*reinterpret_cast<int*>(e + 0) < 0) continue;
                const int key = *reinterpret_cast<int*>(e + 8);
                if (!LooksLikePlayerSkillId(key)) continue;
                if (saneCd(readVal(c, e))) ++score;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (score > bestScore) {
            bestScore = score;
            bestIdx = ci;
        }
    }
    const Cand& c = cands[bestIdx];
    for (int i = 0; i < len && i < 4096; ++i) {
        uint8_t* e =
            reinterpret_cast<uint8_t*>(entries) + 0x20 + static_cast<size_t>(i) * c.stride;
        __try {
            if (*reinterpret_cast<int*>(e + 0) < 0) continue;
            if (*reinterpret_cast<int*>(e + 8) != skillId) continue;
            const int v = readVal(c, e);
            if (v <= 0) return 0.f;
            float sec = static_cast<float>(v);
            if (v > 600 && v <= 600000) sec = static_cast<float>(v) / 1000.f;
            if (sec <= 0.f || sec > 7200.f) return 0.f;
            return sec;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return 0.f;
}

void CastJobFn(void* user) {
    auto* job = reinterpret_cast<CastJob*>(user);
    if (!job) return;
    job->ok = false;
    job->notReady = false;
    job->reason[0] = 0;
    if (!gLocalUser || !gDoActiveSkill) {
        SetJobReason(job, "no_api");
        return;
    }
    EnsureMethodInfos();

    // 主路径：DoActiveSkill(skillId) —— 对标枫星 UseOnClientImmediate 的公开总入口。
    // 内部按技能类型分发 Melee/Shoot/Magic/Prepare/StatChange，再走到 SendSkillUseRequest。
    bool ok = false;
    __try {
        ok = gDoActiveSkill(gLocalUser, job->skillId, 0u, gMiDoActiveSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetJobReason(job, "seh_do_active");
        return;
    }
    if (ok) {
        job->ok = true;
        SetJobReason(job, gMiDoActiveSkill ? "ok_do_active" : "ok_do_active_mi0");
        return;
    }

    // 回退 Prepare：无 MethodInfo 时直调易 SEH（BIN LOG 大量 seh_prepare），直接放弃。
    if (!gPrepare) {
        SetJobReason(job, gMiDoActiveSkill ? "do_false_no_prep" : "do_false_mi0_noprep");
        job->notReady = true;
        return;
    }
    if (!gMiPrepare) {
        SetJobReason(job, "do_false_skip_prep_mi0");
        job->notReady = true;
        return;
    }
    if (!gGetSkill || !gGetSkillLevel) {
        SetJobReason(job, "do_false_no_api");
        job->notReady = true;
        return;
    }
    void* si = ResolveSkillInfoSingleton(GetTickCount());
    if (!LooksLikeHeapPtr(si)) {
        SetJobReason(job, "do_false_no_si");
        job->notReady = true;
        return;
    }
    void* entry = nullptr;
    int level = 0;
    __try {
        level = gGetSkillLevel(gLocalUser, job->skillId, gMiGetSkillLevel);
        entry = gGetSkill(si, job->skillId, gMiGetSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetJobReason(job, "seh_resolve");
        return;
    }
    if (level <= 0) {
        SetJobReason(job, "no_level");
        job->notReady = true;
        return;
    }
    if (!LooksLikeHeapPtr(entry)) {
        SetJobReason(job, "no_entry");
        return;
    }
    __try {
        ok = gPrepare(gLocalUser, entry, level, 0u, gMiPrepare);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SetJobReason(job, "seh_prepare");
        return;
    }
    job->ok = ok;
    if (ok) {
        SetJobReason(job, "ok_prepare");
    } else {
        SetJobReason(job, "prepare_false");
        job->notReady = true;
    }
}

}  // namespace

void Init() {
    gBound = ResolveApi();
    EnsureMethodInfos();
    runtime::LogI("SkillPort",
                  "init bound=%d DoActive=0x%X Prepare=0x%X getSkill=0x%X getLv=0x%X "
                  "miDo=%p miP=%p miLv=%p miGet=%p (SendSkillUse=0x%X reserved)",
                  gBound.load() ? 1 : 0, kRvaDoActiveSkill, kRvaDoActiveSkillPrepare,
                  kRvaSkillInfoGetSkill, kRvaGetSkillLevel, (void*)gMiDoActiveSkill,
                  (void*)gMiPrepare, (void*)gMiGetSkillLevel, (void*)gMiGetSkill,
                  kRvaSendSkillUseRequest);
}

void Shutdown() {
    gLocalUser = nullptr;
    gSkillInfo = nullptr;
    gMiGetSkillLevel = nullptr;
    gMiGetSkill = nullptr;
    gMiDoActiveSkill = nullptr;
    gMiPrepare = nullptr;
    gMiGetRemainTime = nullptr;
    gGetRemainTime = nullptr;
    gBound = false;
}

bool EnsureBound() {
    const DWORD now = GetTickCount();
    if (!ResolveApi()) return false;
    const bool lu = TryResolveLocalUser(now);
    const bool wm = world::EnsureBound();
    (void)ResolveSkillInfoSingleton(now);
    gBound = lu;
    return lu && wm;
}

bool Ready() { return gBound && LocalUserStillAlive(); }

int ListActiveSkills(ActiveSkill* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!EnsureBound() || !gLocalUser) return 0;
    void* list = ReadPtr(gLocalUser, kOffAffectedList);
    const int n = ListSize(list);
    void* items = ListItems(list);
    int w = 0;
    const int arrLen = items ? ReadI32(items, 0x18) : 0;
    int lim = n;
    if (lim < 0) lim = 0;
    if (arrLen > 0 && lim > arrLen) lim = arrLen;
    for (int i = 0; i < lim && i < 256 && w < cap; ++i) {
        void* e = ArrayAtPtr(items, i);
        if (!LooksLikeHeapPtr(e)) continue;
        const int id = ReadI32(e, kOffAffSkillId);
        if (id <= 0) continue;
        out[w].skillId = id;
        out[w].startTime = ReadI32(e, kOffAffStartTime);
        ++w;
    }
    if (w == 0) {
        const DWORD now = GetTickCount();
        if (now - gLastAffDiagMs >= 10000) {
            gLastAffDiagMs = now;
            void* ss = nullptr;
            if (void* wm = world::GetWorldManager()) ss = ReadPtr(wm, kOffWmSecondaryStat);
            void* e0 = (n > 0) ? ArrayAtPtr(items, 0) : nullptr;
            const int id0 = LooksLikeHeapPtr(e0) ? ReadI32(e0, kOffAffSkillId) : 0;
            runtime::LogW("SkillPort",
                          "aff empty list=%p size=%d items=%p arrLen=%d lu=%p ss=%p e0=%p id0=%d",
                          list, n, items, arrLen, gLocalUser, ss, e0, id0);
        }
    }
    return w;
}

bool IsSkillActive(int skillId, float* outRemainSec) {
    if (outRemainSec) *outRemainSec = 0.f;
    if (skillId <= 0) return false;
    ActiveSkill buf[kMaxActiveSkills]{};
    const int n = ListActiveSkills(buf, kMaxActiveSkills);
    for (int i = 0; i < n; ++i) {
        if (buf[i].skillId != skillId) continue;
        const float rem = RemainFromSecondaryStat(skillId);
        if (outRemainSec) *outRemainSec = rem;
        return true;
    }
    // 辅证：肉眼有图标时 AffectedList 偶发仍空，SecondaryStat.rXxx_==skillId 可验身。
    const float ssRem = RemainFromSecondaryStat(skillId);
    if (ssRem > 0.f) {
        if (outRemainSec) *outRemainSec = ssRem;
        return true;
    }
    return false;
}

int GetSkillLevel(int skillId) {
    if (skillId <= 0 || !EnsureBound() || !gLocalUser || !gGetSkillLevel) return 0;
    EnsureMethodInfos();
    __try {
        return gGetSkillLevel(gLocalUser, skillId, gMiGetSkillLevel);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void* GetSkillEntry(int skillId) {
    if (skillId <= 0) return nullptr;
    if (!EnsureBound()) return nullptr;
    EnsureMethodInfos();
    void* si = ResolveSkillInfoSingleton(GetTickCount());
    if (!LooksLikeHeapPtr(si) || !gGetSkill) return nullptr;
    void* entry = nullptr;
    __try {
        entry = gGetSkill(si, skillId, gMiGetSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return LooksLikeHeapPtr(entry) ? entry : nullptr;
}

bool ResolveSkillName(int skillId, char* out, int outSz) {
    if (!out || outSz <= 0) return false;
    out[0] = 0;
    if (skillId <= 0) return false;

    char idBuf[32]{};
    snprintf(idBuf, sizeof(idBuf), "%d", skillId);

    // offline-first：稳定繁中、少碰 Il2Cpp 字符串偏移；表外再 RUNTIME。
    {
        const xcat::SkillNamesPack& pack = xcat::GetSharedSkillNames(x::runtime::GetBinDir());
        const char* offline = xcat::SkillNameLookupById(pack, skillId);
        if (offline && offline[0]) {
            strncpy_s(out, static_cast<size_t>(outSz), offline, _TRUNCATE);
            return true;
        }
    }

    const DWORD now = GetTickCount();
    void* si = ResolveSkillInfoSingleton(now);
    EnsureMethodInfos();
    if (!si || !gGetSkill) {
        strncpy_s(out, static_cast<size_t>(outSz), idBuf, _TRUNCATE);
        return false;
    }
    void* entry = nullptr;
    __try {
        entry = gGetSkill(si, skillId, gMiGetSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        entry = nullptr;
    }
    if (!LooksLikeHeapPtr(entry)) {
        strncpy_s(out, static_cast<size_t>(outSz), idBuf, _TRUNCATE);
        return false;
    }
    void* nameObj = ReadPtr(entry, kOffSkillName);
    if (ReadIl2CppString(nameObj, out, outSz) && out[0] && std::strcmp(out, idBuf) != 0) {
        return true;
    }
    strncpy_s(out, static_cast<size_t>(outSz), idBuf, _TRUNCATE);
    return false;
}

int ListLearnedSkills(SkillInfoLite* out, int cap) {
    if (!out || cap <= 0) return 0;
    if (!EnsureBound()) return 0;
    void* cd = CharacterData();
    if (!cd) return 0;
    EnsureMethodInfos();

    ActiveSkill active[kMaxActiveSkills]{};
    const int activeN = ListActiveSkills(active, kMaxActiveSkills);
    auto isActive = [&](int id) {
        for (int i = 0; i < activeN; ++i)
            if (active[i].skillId == id) return true;
        return false;
    };
    auto already = [&](int id, int n) {
        for (int j = 0; j < n; ++j)
            if (out[j].skillId == id) return true;
        return false;
    };
    auto pushLearned = [&](int key, int val, int& w) {
        if (w >= cap) return;
        if (!LooksLikePlayerSkillId(key) || !LooksLikeSkillLevel(val)) return;
        if (already(key, w)) return;
        SkillInfoLite& s = out[w];
        s = {};
        s.skillId = key;
        s.level = val;
        s.learned = true;
        s.active = isActive(key);
        s.remainBuffSec = s.active ? RemainFromSecondaryStat(key) : 0.f;
        s.remainCooldownSec = CooltimeRemainSec(key);
        s.cooldownSec = s.remainCooldownSec;
        snprintf(s.code, sizeof(s.code), "%d", key);
        ResolveSkillName(key, s.name, sizeof(s.name));
        ++w;
    };

    int w = 0;
    void* skillRec = ReadPtr(cd, kOffSkillRecord);
    void* skillRecEx = ReadPtr(cd, kOffSkillRecordEx);
    ForEachDictIntInt(skillRec, [&](int key, int val) { pushLearned(key, val, w); });
    ForEachDictIntInt(skillRecEx, [&](int key, int val) { pushLearned(key, val, w); });

    // 故意不再扫 SkillInfo._dictionarySkill + GetSkillLevel：
    // BIN 曾出现 SkillRecord listed=3，fallback 再 hit=4 → 把技能书不显示的活动/隐藏技
    // （1009 竹竿天擊等）塞进多发列表。列表以角色 SkillRecord/Ex 为准，对齐技能书已学。

    const DWORD now = GetTickCount();
    if (!gLastDictDiagMs || static_cast<DWORD>(now - gLastDictDiagMs) > 15000u) {
        gLastDictDiagMs = now;
        runtime::LogI("SkillPort", "SkillRecord count=%d ex=%d listed=%d (record-only)",
                      DictIntIntCount(skillRec), DictIntIntCount(skillRecEx), w);
    }

    for (int i = 0; i < activeN && w < cap; ++i) {
        const int id = active[i].skillId;
        if (already(id, w)) continue;
        const int lv = GetSkillLevel(id);
        if (lv <= 0 && !LooksLikePlayerSkillId(id)) continue;
        SkillInfoLite& s = out[w];
        s = {};
        s.skillId = id;
        s.level = lv > 0 ? lv : 1;
        s.learned = lv > 0;
        s.active = true;
        s.remainBuffSec = RemainFromSecondaryStat(id);
        s.remainCooldownSec = CooltimeRemainSec(id);
        s.cooldownSec = s.remainCooldownSec;
        snprintf(s.code, sizeof(s.code), "%d", id);
        ResolveSkillName(id, s.name, sizeof(s.name));
        ++w;
    }
    return w;
}

float GetSkillCooldownRemainSec(int skillId) {
    if (skillId <= 0 || !EnsureBound()) return 0.f;
    return CooltimeRemainSec(skillId);
}

bool CastSkill(int skillId, bool* notReady, char* outReason, int reasonSz) {
    auto setReason = [&](const char* r) {
        if (outReason && reasonSz > 0) strncpy_s(outReason, reasonSz, r ? r : "", _TRUNCATE);
    };
    if (notReady) *notReady = false;
    if (skillId <= 0) {
        setReason("bad_id");
        return false;
    }
    if (!EnsureBound() || !LocalUserStillAlive()) {
        if (notReady) *notReady = true;
        setReason("no_lu");
        return false;
    }
    // 主路径 DoActiveSkill 不依赖 SkillInfo；SI 仅 Prepare 回退需要（CastJobFn 内再解析）。
    // 此前硬门禁 no_si 会在 SI 单例未绑定时把全部施放挡死（BIN LOG 70× reason=no_si）。
    (void)ResolveSkillInfoSingleton(GetTickCount());
    EnsureMethodInfos();
    if (!runtime::main_thread::Ensure()) {
        if (notReady) *notReady = true;
        setReason("pump_fail");
        return false;
    }
    CastJob job{};
    job.skillId = skillId;
    if (!runtime::main_thread::InvokeAndWait(&CastJobFn, &job, kJobWaitMs)) {
        if (notReady) *notReady = true;
        setReason("invoke_timeout");
        return false;
    }
    if (notReady) *notReady = job.notReady && !job.ok;
    setReason(job.reason[0] ? job.reason : (job.ok ? "ok" : "fail"));
    return job.ok;
}

}  // namespace x::features::ports::skill
