// Classic TWMS worldmap_marker_travel
//
// 对照枫星：世界地图 Spot 双击 → 确认框 → travel::RequestGoto。
// 实现差异（Il2Cpp / 非 Lua）：
//   - UIWorldMapItem 无原生 OnDoubleClick；OnPointerDown 用 clickCount / 系统双击间隔自检。
//   - UpdateView 缓存 Spot 的 MapNo[0] + 地图名，双击时优先用图号 goto。
//   - 确认：UIUtilDialog.YesNo + 原生 System.Action（对照枫星 TextConfirm）。
//   - 瞬移石开的是 UIMapTransferDialog，不是 UIWorldMap → 无需石头/非石头门控。
// 防漂移：Spot/MapListData/clickCount 字段走 hash + field_get_offset；dump 常量仅 fallback。
// OnPointerDown 仍可能 abs 钉 RVA（BIN：纯 MI 收不到点击）——方法侧 ResolveMi 另有哈希/kind。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "worldmap_marker_travel.h"

#include "../travel/travel.h"
#include "../ports/travel_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/anchor_lamps.h"

#include "../../../common/xcat_map_names.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace x::features::worldmap_marker_travel {
namespace {

using x::runtime::il2cpp::AtRva;
using x::runtime::il2cpp::FindClass;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// UIWorldMapItem（Prefab 字段形：三 string@0x70/78/80 + mapId@0x88）
constexpr char kItemClass[] =
    "ad787f7539c555758fcfa3f016618b57599c2d3325193a7330d4703b27b0e7a";
constexpr uint32_t kRvaUpdateView = 0x7E4130;  // remapped 2026-08-04
constexpr uint32_t kRvaOnPointerDown = 0x7E6980;  // remapped 2026-08-04

// UIUtilDialog（非 Ex）：YesNo(string,Action,Action,…) / Notice(string,string,bool…)
constexpr char kUtilDialogClass[] =
    "a19ac73ab18613eb5ac5dff4069bb49bbba0f54afc09f03e68e37bf60620a9d";
constexpr uint32_t kRvaYesNo = 0x746B30;  // remapped 2026-08-04
constexpr uint32_t kRvaNotice = 0x74ACB0;  // remapped 2026-08-04

constexpr char kHashUpdateView[] =
    "a8fad7f2ed70099febb1d323fa669ef28cce665afc7891ed60d25b88ad6b1f6";
constexpr char kHashYesNo[] =
    "bf834074b97070ec325df8cfe5d0b853e076124dcae9f57c7b0fab5c55a49ed";
constexpr char kHashNotice[] =
    "eaf272a2779e899e65839c1b95804192c24634ddf628a739da4fe3e6fc5fb7b";

// dump 验证 fallback（remount 2026-08-04；UpdateView 写回 +0x70/78/80/88）
constexpr size_t kFbMapDesc = 0x70;
constexpr size_t kFbMapName = 0x78;
constexpr size_t kFbStreetName = 0x80;
constexpr size_t kFbCachedMapId = 0x88;
constexpr size_t kFbMapNoList = 0x20;   // List<int> MapNo
constexpr size_t kFbMapTitle = 0x28;    // string Title
constexpr size_t kFbListItems = 0x10;   // List._items（BCL，通常不漂）
constexpr size_t kFbListSize = 0x18;    // List._size
constexpr size_t kFbArrayFirst = 0x20;  // Il2CppArray first element
constexpr size_t kFbPointerClickCount = 0x178;

// Spot 字段哈希（dump.cs TypeDefIndex 630）
constexpr char kHashMapDesc[] =
    "ff5aa6b9e1ba538938e4eccc001fd50133fd01746c9b5e6f23294390472235e";
constexpr char kHashMapName[] =
    "c0fda8aeaef5b552762aac94f62b86b4dd161080aa6412c93a7d5886f30f814";
constexpr char kHashStreetName[] =
    "b09a3133fd8c7df4686a400b824c7ccaafc2df5e7e28eb2017dfe71480c9ad1";
constexpr char kHashCachedMapId[] =
    "c54b695d6894278b0804079a21f4d81f18c2b7c6cabb583321d88bdf5c494a3";

// MapListData 嵌套类 + 属性 backing 字段（TypeDefIndex 2183）
constexpr char kMapListDataClass[] =
    "c49d389d0ddc4fcaa210298cfcc11f5f6b823b73cd4176111c21a377dda3a78."
    "d3094bad13b41c2584d94ed141964cb9c456b5858cd1d5cd7efc929a663519c";
constexpr char kMapListDataClassSlash[] =
    "c49d389d0ddc4fcaa210298cfcc11f5f6b823b73cd4176111c21a377dda3a78/"
    "d3094bad13b41c2584d94ed141964cb9c456b5858cd1d5cd7efc929a663519c";
constexpr char kMapListDataNested[] =
    "d3094bad13b41c2584d94ed141964cb9c456b5858cd1d5cd7efc929a663519c";
constexpr char kHashMapNoList[] =
    "<d5f66233c6d6a062d486d16835f48b098614667386e61f54141e5632f8a1b87>k__BackingField";
constexpr char kHashMapTitle[] =
    "<af854d763e9f52b621719100be791eaf743a0c456964751328622d29649a37c>k__BackingField";
constexpr char kHashClickCount[] = "<clickCount>k__BackingField";
constexpr DWORD kDblClickMsMin = 400;
constexpr DWORD kDblClickMsMax = 800;
constexpr DWORD kConfirmReopenMs = 300;
constexpr DWORD kInstallRetryMs = 3000;
constexpr size_t kSpotCacheMax = 256;
constexpr DWORD kClickProbeLogMs = 3000;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invoker;
    const void* nameOrHandle;
};

// UpdateView arity=8（不含 this/MethodInfo）：3×string + MapListData* + 2×int + Object** + Vector2*
using FnUpdateView = void (*)(void* self, void* street, void* mapName, void* mapDesc,
                              void* mapListData, int32_t mapId, int32_t extraInt, void** outObj,
                              void* outVec2, const void* method);
using FnOnPointerDown = void (*)(void* self, void* eventData, const void* method);

struct SpotInfo {
    int mapId = 0;
    char label[160]{};  // 短名优先；也可能暂存简介供反查
};

std::mutex gMu;
std::unordered_map<void*, SpotInfo> gSpotByItem;
std::unordered_map<void*, DWORD> gLastClickMs;

std::atomic<bool> gInstalled{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};

void* gItemKlass = nullptr;
void* gUtilDlgKlass = nullptr;
void* gActionKlass = nullptr;
MethodInfoHead* gMiUpdate = nullptr;
MethodInfoHead* gMiDown = nullptr;
MethodInfoHead* gMiYesNo = nullptr;
MethodInfoHead* gMiNotice = nullptr;
FnUpdateView gOrigUpdate = nullptr;
FnOnPointerDown gOrigDown = nullptr;
DWORD gLastInstallTry = 0;

// OnPointerDown 仅有 MethodInfo 数据 xref、无 E8 直调；但 BIN 显示 MI 换桩后点击仍不进 Hook。
// 对原生入口做 abs jmp（14B），保证任意 invoker/MI 只要落到 RVA 就能进 Hook。
constexpr size_t kDownSteal = 16;  // push rsi/rdi + sub rsp,58h + mov rsi,rcx + lea rax (完整指令)
struct AbsHookState {
    void* target = nullptr;
    void* trampoline = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};
AbsHookState gDownAbs{};

using FnFieldFromName = void* (*)(void* klass, const char* name);
using FnFieldGetOffset = size_t (*)(void* field);
FnFieldFromName gFieldFromName = nullptr;
FnFieldGetOffset gFieldGetOffset = nullptr;
size_t gOffMethodPtr = 0;
size_t gOffInvokeImpl = 0;
size_t gOffExtraArg = 0;
size_t gOffMethodCode = 0;

struct SpotFieldOff {
    size_t mapDesc = kFbMapDesc;
    size_t mapName = kFbMapName;
    size_t streetName = kFbStreetName;
    size_t cachedMapId = kFbCachedMapId;
    size_t mapNoList = kFbMapNoList;
    size_t mapTitle = kFbMapTitle;
    size_t listItems = kFbListItems;
    size_t listSize = kFbListSize;
    size_t arrayFirst = kFbArrayFirst;
    size_t pointerClickCount = kFbPointerClickCount;
    bool tried = false;
    const char* path = "fallback";  // meta | meta-partial | fallback
};
SpotFieldOff gSpotOff{};
bool gDelegateOffOk = false;

void* gYesAction = nullptr;
void* gNoAction = nullptr;
void* gOkAction = nullptr;
uint32_t gYesHandle = 0;
uint32_t gNoHandle = 0;
uint32_t gOkHandle = 0;

using FnYesNo = void* (*)(void* sMsg, void* yes, void* no, void* sSndName, uint8_t autoSep,
                          uint8_t tightLine, uint8_t extraA, uint8_t extraB, const void* method);
using FnNotice = void* (*)(void* sMsg, void* sSub, uint8_t a, uint8_t b, uint8_t c, void* ok,
                           const void* method);

std::atomic<uint32_t> gConfirmGen{0};
std::atomic<bool> gConfirming{false};
DWORD gConfirmAtMs = 0;
char gPendingTarget[96]{};
char gPendingLabel[96]{};
char gConfirmKey[96]{};

void SafeRuntimeClassInit(void* klass);

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
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

int FirstMapNo(void* mapListData) {
    if (!LooksLikeHeapPtr(mapListData)) return 0;
    void* list = ReadPtr(mapListData, gSpotOff.mapNoList);
    if (!LooksLikeHeapPtr(list)) return 0;
    const int size = ReadI32(list, gSpotOff.listSize);
    if (size <= 0 || size > 64) return 0;
    void* items = ReadPtr(list, gSpotOff.listItems);
    if (!LooksLikeHeapPtr(items)) return 0;
    const int v = ReadI32(items, gSpotOff.arrayFirst);
    return v > 0 ? v : 0;
}

bool LooksLikeMapDesc(const char* s) {
    if (!s || !s[0]) return false;
    // 实机误把 Desc 当名：「位於…村落，有武器店…。」
    const size_t n = std::strlen(s);
    if (n >= 40) return true;
    for (const char* p = s; *p; ++p) {
        const unsigned char c0 = static_cast<unsigned char>(p[0]);
        const unsigned char c1 = static_cast<unsigned char>(p[1]);
        const unsigned char c2 = static_cast<unsigned char>(p[2]);
        // UTF-8：。 U+3002 = E3 80 82；， U+FF0C = EF BC 8C
        if (c0 == 0xE3 && c1 == 0x80 && c2 == 0x82) return true;
        if (c0 == 0xEF && c1 == 0xBC && c2 == 0x8C) return true;
        if (*p == '.' && n > 16) return true;
    }
    return false;
}

int PickMapId(int paramMapId, void* mapListData, void* self) {
    (void)self;
    if (paramMapId > 0) return paramMapId;
    return FirstMapNo(mapListData);
}

void PickLabel(void* mapNameStr, void* mapListData, void* self, char* out, int outSz) {
    if (!out || outSz <= 0) return;
    out[0] = 0;
    char tmp[96]{};
    if (ReadIl2CppString(mapNameStr, tmp, sizeof(tmp)) && tmp[0] && !LooksLikeMapDesc(tmp)) {
        strncpy_s(out, outSz, tmp, _TRUNCATE);
        return;
    }
    if (LooksLikeHeapPtr(mapListData)) {
        if (ReadIl2CppString(ReadPtr(mapListData, gSpotOff.mapTitle), tmp, sizeof(tmp)) && tmp[0] &&
            !LooksLikeMapDesc(tmp)) {
            strncpy_s(out, outSz, tmp, _TRUNCATE);
            return;
        }
    }
    if (self && ReadIl2CppString(ReadPtr(self, gSpotOff.mapName), tmp, sizeof(tmp)) && tmp[0] &&
        !LooksLikeMapDesc(tmp)) {
        strncpy_s(out, outSz, tmp, _TRUNCATE);
        return;
    }
    if (ReadIl2CppString(mapNameStr, tmp, sizeof(tmp)) && tmp[0]) {
        strncpy_s(out, outSz, tmp, _TRUNCATE);
        return;
    }
    if (self && ReadIl2CppString(ReadPtr(self, gSpotOff.mapName), tmp, sizeof(tmp)) && tmp[0]) {
        strncpy_s(out, outSz, tmp, _TRUNCATE);
    }
}

void CacheSpot(void* item, int mapId, const char* label) {
    if (!item) return;
    std::lock_guard<std::mutex> lock(gMu);
    if (gSpotByItem.size() >= kSpotCacheMax && !gSpotByItem.count(item)) {
        gSpotByItem.clear();
        gLastClickMs.clear();
    }
    SpotInfo& s = gSpotByItem[item];
    if (mapId > 0) s.mapId = mapId;
    if (label && label[0]) {
        strncpy_s(s.label, label, _TRUNCATE);
    }
}

bool LookupSpot(void* item, SpotInfo* out) {
    if (!item || !out) return false;
    std::lock_guard<std::mutex> lock(gMu);
    auto it = gSpotByItem.find(item);
    if (it == gSpotByItem.end()) return false;
    *out = it->second;
    return out->mapId > 0 || out->label[0] != '\0';
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods || !e.ga) return nullptr;
    const uintptr_t want = reinterpret_cast<uintptr_t>(e.ga) + rva;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* miRaw = e.classGetMethods(klass, &iter);
            if (!miRaw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(miRaw);
            void* mp = nullptr;
            __try {
                mp = mi->methodPointer;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }
            if (reinterpret_cast<uintptr_t>(mp) == want) return mi;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(klass, &iter);
            if (!raw) break;
            const char* nm = e.methodGetName(raw);
            if (nm && strcmp(nm, name) == 0) {
                mi = reinterpret_cast<MethodInfoHead*>(raw);
                if (mi && mi->methodPointer) return mi;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
    if (outPath) *outPath = mr.path;
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = mi->methodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        mi->methodPointer = hook;
        if (mi->virtualMethodPointer == orig) mi->virtualMethodPointer = hook;
        *outOrig = orig;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return ok;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    __try {
        void* cur = mi->methodPointer;
        mi->methodPointer = orig;
        if (mi->virtualMethodPointer == cur) mi->virtualMethodPointer = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    // mov rax, imm64 ; jmp rax
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

bool InstallAbsHook(AbsHookState* st, void* target, void* hook, size_t steal) {
    if (!st || !target || !hook || steal < 14 || steal > sizeof(st->saved)) return false;
    if (st->active) return true;
    void* tramp = VirtualAlloc(nullptr, steal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(st->saved, target, steal);
    memcpy(tramp, target, steal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + steal,
                reinterpret_cast<uint8_t*>(target) + steal);
    DWORD old = 0;
    if (!VirtualProtect(target, steal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    WriteAbsJmp(target, hook);
    for (size_t i = 14; i < steal; ++i) reinterpret_cast<uint8_t*>(target)[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, steal);
    VirtualProtect(target, steal, old, &old);
    st->target = target;
    st->trampoline = tramp;
    st->stolen = steal;
    st->active = true;
    return true;
}

void RemoveAbsHook(AbsHookState* st) {
    if (!st || !st->active || !st->target) return;
    DWORD old = 0;
    if (VirtualProtect(st->target, st->stolen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(st->target, st->saved, st->stolen);
        FlushInstructionCache(GetCurrentProcess(), st->target, st->stolen);
        VirtualProtect(st->target, st->stolen, old, &old);
    }
    if (st->trampoline) VirtualFree(st->trampoline, 0, MEM_RELEASE);
    st->trampoline = nullptr;
    st->target = nullptr;
    st->stolen = 0;
    st->active = false;
}

void CallOrigDown(void* self, void* eventData, const void* method) {
    FnOnPointerDown orig = gOrigDown;
    if (!orig) return;
    __try {
        orig(self, eventData, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void CallOrigUpdate(void* self, void* street, void* mapName, void* mapDesc, void* mapListData,
                    int32_t mapId, int32_t extraInt, void** outObj, void* outVec2,
                    const void* method) {
    FnUpdateView orig = gOrigUpdate;
    if (!orig) return;
    __try {
        orig(self, street, mapName, mapDesc, mapListData, mapId, extraInt, outObj, outVec2, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ShowTravelNotice(const char* msgUtf8);
bool ShowTravelConfirm(const char* target, const char* label, int hops = -2);

void Hook_UpdateView(void* self, void* street, void* mapName, void* mapDesc, void* mapListData,
                     int32_t mapId, int32_t extraInt, void** outObj, void* outVec2,
                     const void* method) {
    (void)street;
    (void)mapDesc;
    (void)extraInt;
    CallOrigUpdate(self, street, mapName, mapDesc, mapListData, mapId, extraInt, outObj, outVec2,
                   method);

    const int spotMap = PickMapId(mapId, mapListData, self);
    char label[160]{};
    PickLabel(mapName, mapListData, self, label, sizeof(label));
    if (!label[0]) {
        if (!ReadIl2CppString(mapDesc, label, sizeof(label)))
            (void)ReadIl2CppString(ReadPtr(self, gSpotOff.mapDesc), label, sizeof(label));
    }
    CacheSpot(self, spotMap, label);

    static DWORD s_lastUv = 0;
    static uint32_t s_uvN = 0;
    ++s_uvN;
    const DWORD now = GetTickCount();
    if (s_uvN <= 3 || now - s_lastUv >= 2000) {
        s_lastUv = now;
        x::runtime::LogI("WorldMapTravel", "UpdateView ok n=%u mapId=%d label=%s", s_uvN, spotMap,
                         label[0] ? label : "-");
    }
}

void FireGotoFromItem(void* self) {
    SpotInfo spot{};
    char nameBuf[160]{};
    char key[32]{};
    int mapId = 0;

    if (LookupSpot(self, &spot)) {
        mapId = spot.mapId;
        if (spot.label[0]) strncpy_s(nameBuf, spot.label, _TRUNCATE);
    }
    if (mapId <= 0) {
        // 缓存未命中：再试字段侧无法可靠读图号时，靠 label→map_names
        mapId = PickMapId(0, nullptr, self);
    }
    if (!nameBuf[0] || LooksLikeMapDesc(nameBuf)) {
        char fresh[160]{};
        PickLabel(nullptr, nullptr, self, fresh, sizeof(fresh));
        if (fresh[0]) {
            if (!LooksLikeMapDesc(fresh) || !nameBuf[0]) strncpy_s(nameBuf, fresh, _TRUNCATE);
        } else if (!nameBuf[0]) {
            (void)ReadIl2CppString(ReadPtr(self, gSpotOff.mapDesc), nameBuf, sizeof(nameBuf));
        }
    }
    if (mapId > 0) snprintf(key, sizeof(key), "%d", mapId);

    // 无图号时：用简介/名经 map_names 反查（禁止把简介原文直接 RequestGoto）
    if (!key[0] && nameBuf[0]) {
        const auto& pack = xcat::GetSharedMapNames(x::runtime::GetBinDir());
        const std::string resolved = xcat::MapNamesResolveQuery(pack, nameBuf);
        if (!resolved.empty()) {
            int id = 0;
            try {
                id = std::stoi(resolved);
            } catch (...) {
                id = 0;
            }
            if (id > 0) {
                mapId = id;
                snprintf(key, sizeof(key), "%d", id);
            }
        }
    }

    if (!key[0]) {
        x::runtime::LogW("WorldMapTravel",
                         "双击 Spot 无法解析图号 mapId=%d label=%s，跳过", mapId,
                         nameBuf[0] ? nameBuf : "-");
        char tip[192]{};
        if (nameBuf[0])
            snprintf(tip, sizeof(tip), "【超级赶路】\n无法解析地图编号\n（%s）", nameBuf);
        else
            snprintf(tip, sizeof(tip), "【超级赶路】\n无法解析地图编号");
        (void)ShowTravelNotice(tip);
        return;
    }

    char show[160]{};
    if (nameBuf[0] && !LooksLikeMapDesc(nameBuf)) {
        strncpy_s(show, nameBuf, _TRUNCATE);
    } else {
        const auto& pack = xcat::GetSharedMapNames(x::runtime::GetBinDir());
        const std::string lab = xcat::MapNamesLabelById(pack, mapId > 0 ? mapId : std::atoi(key));
        if (!lab.empty()) strncpy_s(show, lab.c_str(), _TRUNCATE);
        else if (nameBuf[0]) strncpy_s(show, nameBuf, _TRUNCATE);
    }

    // 同盘可达性预检：不可达 / 已在目标 → 游戏内 Notice，不弹确认赶路
    const std::string cur = ports::travel::CurrentMapKey();
    if (!cur.empty()) {
        const int hops = travel::PathHopCount(cur.c_str(), key);
        if (hops == 0) {
            char tip[192]{};
            if (show[0])
                snprintf(tip, sizeof(tip), "【超级赶路】\n已在目标地图\n%s\n(%s)", show, key);
            else
                snprintf(tip, sizeof(tip), "【超级赶路】\n已在目标地图\n%s", key);
            x::runtime::LogI("WorldMapTravel", "已在目标 [%s]，弹 Notice", key);
            (void)ShowTravelNotice(tip);
            return;
        }
        if (hops < 0) {
            char tip[220]{};
            if (show[0])
                snprintf(tip, sizeof(tip),
                         "【超级赶路】\n不可达（同盘无路径）\n%s\n(%s)\n可能需手动过港口/换板块",
                         show, key);
            else
                snprintf(tip, sizeof(tip),
                         "【超级赶路】\n不可达（同盘无路径）\n%s\n可能需手动过港口/换板块", key);
            x::runtime::LogW("WorldMapTravel", "不可达 [%s] from=%s，弹 Notice", key, cur.c_str());
            (void)ShowTravelNotice(tip);
            return;
        }
        x::runtime::LogI("WorldMapTravel", "世界地图 Spot 双击 → 确认框 [%s]%s%s hops=%d", key,
                         show[0] ? " " : "", show[0] ? show : "", hops);
        (void)ShowTravelConfirm(key, show[0] ? show : nullptr, hops);
        return;
    }

    x::runtime::LogI("WorldMapTravel", "世界地图 Spot 双击 → 确认框 [%s]%s%s (无当前图号，跳过预检)",
                     key, show[0] ? " " : "", show[0] ? show : "");
    (void)ShowTravelConfirm(key, show[0] ? show : nullptr);
}

void ClearConfirmLocked() {
    gConfirming.store(false, std::memory_order_release);
    gConfirmAtMs = 0;
    gPendingTarget[0] = 0;
    gPendingLabel[0] = 0;
    gConfirmKey[0] = 0;
}

void ClearConfirm() {
    std::lock_guard<std::mutex> lock(gMu);
    ClearConfirmLocked();
}

void SafeRuntimeClassInit(void* klass) {
    x::runtime::il2cpp::RuntimeClassInit(klass);
}

void WritePtrField(void* obj, size_t off, void* v) {
    if (!obj || !off) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool PlausibleInstanceOff(size_t off) {
    return off >= 0x10 && off < 0x1000;
}

bool EnsureFieldExports() {
    if (gFieldFromName && gFieldGetOffset) return true;
    const auto& e = x::runtime::il2cpp::Get();
    if (e.classGetFieldFromName) gFieldFromName = e.classGetFieldFromName;
    if (e.fieldGetOffset) gFieldGetOffset = e.fieldGetOffset;
    if (gFieldFromName && gFieldGetOffset) return true;
    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    if (!ga) return false;
    if (!gFieldFromName)
        gFieldFromName =
            reinterpret_cast<FnFieldFromName>(GetProcAddress(ga, "il2cpp_class_get_field_from_name"));
    if (!gFieldGetOffset)
        gFieldGetOffset =
            reinterpret_cast<FnFieldGetOffset>(GetProcAddress(ga, "il2cpp_field_get_offset"));
    return gFieldFromName && gFieldGetOffset;
}

// Returns true when metadata supplied a plausible offset (may equal fallback).
bool FieldOffOrFb(void* klass, const char* fieldHash, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !fieldHash || !EnsureFieldExports()) return false;
    void* field = nullptr;
    __try {
        field = gFieldFromName(klass, fieldHash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!field) return false;
    size_t off = 0;
    __try {
        off = gFieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!PlausibleInstanceOff(off)) return false;
    *out = off;
    return true;
}

void* FindMapListDataKlass() {
    void* k = FindClass("", kMapListDataClass);
    if (!k) k = FindClass("", kMapListDataClassSlash);
    if (!k) k = FindClass("", kMapListDataNested);
    return k;
}

void EnsureSpotFieldOffsets(void* itemKlass) {
    if (gSpotOff.tried) return;
    gSpotOff.tried = true;

    if (!EnsureFieldExports()) {
        x::runtime::LogW("WorldMapTravel", "offsets path=fallback (field exports miss)");
        return;
    }

    void* pedKlass = FindClass("UnityEngine.EventSystems", "PointerEventData");
    void* mldKlass = FindMapListDataKlass();

    int hits = 0;
    const int want = 7;  // Spot×4 + MapList×2 + clickCount（BCL List 偏移不计入）
    size_t oDesc = kFbMapDesc, oName = kFbMapName, oStreet = kFbStreetName, oId = kFbCachedMapId;
    size_t oList = kFbMapNoList, oTitle = kFbMapTitle, oClick = kFbPointerClickCount;

    if (FieldOffOrFb(itemKlass, kHashMapDesc, kFbMapDesc, &oDesc)) ++hits;
    if (FieldOffOrFb(itemKlass, kHashMapName, kFbMapName, &oName)) ++hits;
    if (FieldOffOrFb(itemKlass, kHashStreetName, kFbStreetName, &oStreet)) ++hits;
    if (FieldOffOrFb(itemKlass, kHashCachedMapId, kFbCachedMapId, &oId)) ++hits;
    if (FieldOffOrFb(mldKlass, kHashMapNoList, kFbMapNoList, &oList)) ++hits;
    if (FieldOffOrFb(mldKlass, kHashMapTitle, kFbMapTitle, &oTitle)) ++hits;
    if (FieldOffOrFb(pedKlass, kHashClickCount, kFbPointerClickCount, &oClick)) ++hits;

    gSpotOff.mapDesc = oDesc;
    gSpotOff.mapName = oName;
    gSpotOff.streetName = oStreet;
    gSpotOff.cachedMapId = oId;
    gSpotOff.mapNoList = oList;
    gSpotOff.mapTitle = oTitle;
    gSpotOff.pointerClickCount = oClick;
    gSpotOff.listItems = x::runtime::il2cpp_container::OffListItems();
    gSpotOff.listSize = x::runtime::il2cpp_container::OffListSize();
    gSpotOff.arrayFirst = x::runtime::il2cpp_container::OffArrayData();
    gSpotOff.path = hits == want ? "meta" : (hits ? "meta-partial" : "fallback");

    x::runtime::LogI(
        "WorldMapTravel",
        "offsets path=%s hits=%d/%d desc=0x%zx name=0x%zx street=0x%zx id=0x%zx "
        "mapNo=0x%zx title=0x%zx click=0x%zx",
        gSpotOff.path, hits, want, gSpotOff.mapDesc, gSpotOff.mapName, gSpotOff.streetName,
        gSpotOff.cachedMapId, gSpotOff.mapNoList, gSpotOff.mapTitle, gSpotOff.pointerClickCount);
}

bool ResolveDelegateOffsets() {
    if (gDelegateOffOk) return true;
    if (!EnsureFieldExports()) return false;
    void* delKlass = FindClass("System", "Delegate");
    if (!delKlass) return false;
    void* fMp = gFieldFromName(delKlass, "method_ptr");
    void* fInv = gFieldFromName(delKlass, "invoke_impl");
    void* fEx = gFieldFromName(delKlass, "extra_arg");
    void* fCode = gFieldFromName(delKlass, "method_code");
    if (!fMp || !fInv || !fEx || !fCode) return false;
    gOffMethodPtr = gFieldGetOffset(fMp);
    gOffInvokeImpl = gFieldGetOffset(fInv);
    gOffExtraArg = gFieldGetOffset(fEx);
    gOffMethodCode = gFieldGetOffset(fCode);
    if (!gOffMethodPtr || !gOffInvokeImpl) return false;
    gDelegateOffOk = true;
    return true;
}

void WireActionTargets(void* action, void* nativeFn) {
    if (!action || !nativeFn || !gDelegateOffOk) return;
    WritePtrField(action, gOffMethodPtr, nativeFn);
    WritePtrField(action, gOffInvokeImpl, nativeFn);
    WritePtrField(action, gOffExtraArg, action);
    WritePtrField(action, gOffMethodCode, nativeFn);
}

void __fastcall OnConfirmYes(void*, void*) {
    char target[96]{};
    char label[96]{};
    {
        std::lock_guard<std::mutex> lock(gMu);
        if (!gConfirming.load(std::memory_order_relaxed)) return;
        strncpy_s(target, gPendingTarget, _TRUNCATE);
        strncpy_s(label, gPendingLabel, _TRUNCATE);
        ClearConfirmLocked();
    }
    if (!target[0]) return;
    x::runtime::LogI("WorldMapTravel", "确认赶路 → RequestGoto [%s]%s%s", target,
                     label[0] ? " " : "", label[0] ? label : "");
    travel::RequestGoto(target);
}

void __fastcall OnConfirmNo(void*, void*) {
    std::lock_guard<std::mutex> lock(gMu);
    if (!gConfirming.load(std::memory_order_relaxed)) return;
    x::runtime::LogI("WorldMapTravel", "取消赶路 [%s]", gPendingTarget[0] ? gPendingTarget : "-");
    ClearConfirmLocked();
}

void __fastcall OnNoticeOk(void*, void*) {
    // Notice 仅告知，无状态
}

bool EnsureConfirmActions() {
    if (gYesAction && gNoAction && gOkAction) return true;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.objectNew || !e.gcHandleNew) return false;
    if (!ResolveDelegateOffsets()) return false;
    if (!gActionKlass) gActionKlass = FindClass("System", "Action");
    if (!gActionKlass) return false;
    SafeRuntimeClassInit(gActionKlass);

    void* yes = gYesAction;
    void* no = gNoAction;
    void* ok = gOkAction;
    if (!yes) yes = x::runtime::il2cpp::AllocObject(gActionKlass);
    if (!no) no = x::runtime::il2cpp::AllocObject(gActionKlass);
    if (!ok) ok = x::runtime::il2cpp::AllocObject(gActionKlass);
    if (!yes || !no || !ok) return false;
    WireActionTargets(yes, reinterpret_cast<void*>(&OnConfirmYes));
    WireActionTargets(no, reinterpret_cast<void*>(&OnConfirmNo));
    WireActionTargets(ok, reinterpret_cast<void*>(&OnNoticeOk));
    if (!gYesHandle) {
        const uint32_t hy = e.gcHandleNew(yes, false);
        if (!hy) return false;
        gYesHandle = hy;
        gYesAction = yes;
    }
    if (!gNoHandle) {
        const uint32_t hn = e.gcHandleNew(no, false);
        if (!hn) return false;
        gNoHandle = hn;
        gNoAction = no;
    }
    if (!gOkHandle) {
        const uint32_t ho = e.gcHandleNew(ok, false);
        if (!ho) return false;
        gOkHandle = ho;
        gOkAction = ok;
    }
    return gYesAction && gNoAction && gOkAction;
}

bool EnsureYesNoMi() {
    if (gMiYesNo) return true;
    if (!gUtilDlgKlass) {
        gUtilDlgKlass = FindClass("", kUtilDialogClass);
        if (!gUtilDlgKlass) gUtilDlgKlass = FindClass("Msc.UI", "UIUtilDialog");
    }
    if (!gUtilDlgKlass) return false;
    SafeRuntimeClassInit(gUtilDlgKlass);
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    // static YesNo(string,Action,Action,…) arity=8 — 同形可能多 → 哈希主
    constexpr MethodShape kYn{8, TypeKind::Ptr, false, false, {TypeKind::Ptr, TypeKind::Ptr}};
    using x::runtime::il2cpp_method::ResolvePath;
    ResolvePath pYn{};
    gMiYesNo = ResolveMi(gUtilDlgKlass, kRvaYesNo, kYn, "YesNo", kHashYesNo, &pYn);
    static bool sMethodHitsLogged = false;
    if (!sMethodHitsLogged && gMiYesNo) {
        sMethodHitsLogged = true;
        int hits = (pYn != ResolvePath::Miss) ? 1 : 0;
        // Notice / UpdateView 在各自 Ensure 时再计；此处先报 YesNo。
        x::runtime::LogI("WorldMapTravel", "methods path=%s hits=%d/3 (YesNo; Notice+UV deferred)",
                         hits ? "meta" : "fallback", hits);
    }
    return gMiYesNo != nullptr;
}

bool EnsureNoticeMi() {
    if (gMiNotice) return true;
    if (!gUtilDlgKlass) {
        gUtilDlgKlass = FindClass("", kUtilDialogClass);
        if (!gUtilDlgKlass) gUtilDlgKlass = FindClass("Msc.UI", "UIUtilDialog");
    }
    if (!gUtilDlgKlass) return false;
    SafeRuntimeClassInit(gUtilDlgKlass);
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kNt{6, TypeKind::Ptr, false, false, {TypeKind::Ptr, TypeKind::Ptr}};
    gMiNotice = ResolveMi(gUtilDlgKlass, kRvaNotice, kNt, "Notice", kHashNotice);
    return gMiNotice != nullptr;
}

bool CallYesNoSeh(void* msg, void* yes, void* no, void* snd) {
    if (!gMiYesNo || !gMiYesNo->methodPointer) return false;
    auto* fn = reinterpret_cast<FnYesNo>(gMiYesNo->methodPointer);
    __try {
        fn(msg, yes, no, snd, 0, 0, 0, 0, gMiYesNo);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallNoticeSeh(void* msg, void* sub, void* ok) {
    if (!gMiNotice || !gMiNotice->methodPointer) return false;
    auto* fn = reinterpret_cast<FnNotice>(gMiNotice->methodPointer);
    __try {
        fn(msg, sub, 0, 0, 0, ok, gMiNotice);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MakeIl2CppStrings(const char* msgUtf8, void** outMsg, void** outSnd) {
    *outMsg = nullptr;
    *outSnd = nullptr;
    if (!msgUtf8) return false;
    __try {
        *outMsg = x::runtime::il2cpp::NewString(msgUtf8);
        *outSnd = x::runtime::il2cpp::NewString("");
        return *outMsg != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outMsg = nullptr;
        *outSnd = nullptr;
        return false;
    }
}

// 游戏内确定框（不可达 / 已在目标 / 无法解析）——比 launcher 气泡更显眼。
bool ShowTravelNotice(const char* msgUtf8) {
    if (!msgUtf8 || !msgUtf8[0]) return false;
    if (!EnsureNoticeMi() || !EnsureConfirmActions()) {
        x::runtime::LogW("WorldMapTravel", "Notice 未就绪 mi=%p → 仅日志: %s", (void*)gMiNotice,
                         msgUtf8);
        return false;
    }
    void* msg = nullptr;
    void* sub = nullptr;
    if (!MakeIl2CppStrings(msgUtf8, &msg, &sub)) return false;
    WireActionTargets(gOkAction, reinterpret_cast<void*>(&OnNoticeOk));
    if (!CallNoticeSeh(msg, sub, gOkAction)) {
        x::runtime::LogW("WorldMapTravel", "UIUtilDialog.Notice 调用失败: %s", msgUtf8);
        return false;
    }
    x::runtime::LogI("WorldMapTravel", "已弹出提示框");
    return true;
}

bool ShowTravelConfirm(const char* target, const char* label, int hops) {
    if (!target || !target[0]) return false;
    const bool yesOk = EnsureYesNoMi();
    const bool actOk = EnsureConfirmActions();
    if (!yesOk || !actOk) {
        x::runtime::LogW("WorldMapTravel", "确认框未就绪 yesNo=%p action=%d → 直通 goto",
                         (void*)gMiYesNo, actOk ? 1 : 0);
        travel::RequestGoto(target);
        return true;
    }

    const DWORD now = GetTickCount();
    {
        std::lock_guard<std::mutex> lock(gMu);
        if (gConfirming.load(std::memory_order_relaxed)) {
            const bool same = (std::strcmp(gConfirmKey, target) == 0);
            if (same && now - gConfirmAtMs < kConfirmReopenMs) return true;
        }
        gConfirmGen.fetch_add(1, std::memory_order_relaxed);
        gConfirming.store(true, std::memory_order_release);
        gConfirmAtMs = now;
        strncpy_s(gPendingTarget, target, _TRUNCATE);
        strncpy_s(gConfirmKey, target, _TRUNCATE);
        if (label && label[0])
            strncpy_s(gPendingLabel, label, _TRUNCATE);
        else
            gPendingLabel[0] = 0;
    }

    char msgBuf[220]{};
    if (label && label[0] && std::strcmp(label, target) != 0) {
        if (hops > 0)
            snprintf(msgBuf, sizeof(msgBuf), "【超级赶路】\n前往：%s\n(%s)\n预计 %d 跳", label,
                     target, hops);
        else
            snprintf(msgBuf, sizeof(msgBuf), "【超级赶路】\n前往：%s\n(%s)", label, target);
    } else {
        if (hops > 0)
            snprintf(msgBuf, sizeof(msgBuf), "【超级赶路】\n前往：%s\n预计 %d 跳", target, hops);
        else
            snprintf(msgBuf, sizeof(msgBuf), "【超级赶路】\n前往：%s", target);
    }

    void* msg = nullptr;
    void* snd = nullptr;
    if (!MakeIl2CppStrings(msgBuf, &msg, &snd)) {
        ClearConfirm();
        return false;
    }

    WireActionTargets(gYesAction, reinterpret_cast<void*>(&OnConfirmYes));
    WireActionTargets(gNoAction, reinterpret_cast<void*>(&OnConfirmNo));
    if (!CallYesNoSeh(msg, gYesAction, gNoAction, snd)) {
        ClearConfirm();
        x::runtime::LogW("WorldMapTravel", "UIUtilDialog.YesNo 调用失败 → 直通 goto");
        travel::RequestGoto(target);
        return false;
    }
    x::runtime::LogI("WorldMapTravel", "已弹出确认 [%s] hops=%d", target, hops);
    return true;
}

DWORD DblClickWindowMs() {
    DWORD sys = GetDoubleClickTime();
    if (sys < kDblClickMsMin) sys = kDblClickMsMin;
    if (sys > kDblClickMsMax) sys = kDblClickMsMax;
    return sys;
}

int ReadPointerClickCount(void* eventData) {
    if (!eventData || !LooksLikeHeapPtr(eventData)) return 0;
    int n = 0;
    __try {
        n = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(eventData) + gSpotOff.pointerClickCount);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    return n;
}

bool NoteClickAndIsDouble(void* self, void* eventData, int* outClickCount) {
    const int clickCount = ReadPointerClickCount(eventData);
    if (outClickCount) *outClickCount = clickCount;
    // Unity 在派发 OnPointerDown 前已更新 clickCount；>=2 即系统判定双击
    if (clickCount >= 2) {
        std::lock_guard<std::mutex> lock(gMu);
        gLastClickMs[self] = 0;
        return true;
    }
    const DWORD now = GetTickCount();
    const DWORD win = DblClickWindowMs();
    std::lock_guard<std::mutex> lock(gMu);
    auto it = gLastClickMs.find(self);
    if (it != gLastClickMs.end() && it->second != 0 && now - it->second <= win) {
        it->second = 0;
        return true;
    }
    gLastClickMs[self] = now;
    return false;
}

void Hook_OnPointerDown(void* self, void* eventData, const void* method) {
    int clickCount = 0;
    const bool dbl = NoteClickAndIsDouble(self, eventData, &clickCount);
    CallOrigDown(self, eventData, method);
    if (dbl) {
        x::runtime::LogI("WorldMapTravel", "Spot 双击判定 self=%p clickCount=%d", self, clickCount);
        FireGotoFromItem(self);
        return;
    }
    static DWORD s_lastProbe = 0;
    static uint32_t s_clicks = 0;
    ++s_clicks;
    const DWORD now = GetTickCount();
    if (s_clicks <= 8 || now - s_lastProbe >= kClickProbeLogMs) {
        s_lastProbe = now;
        x::runtime::LogI("WorldMapTravel", "Spot 单击 self=%p clickCount=%d n=%u winMs=%u", self,
                         clickCount, s_clicks, DblClickWindowMs());
    }
}

bool TryInstall() {
    if (gInstalled.load()) return true;
    if (!travel::IsFeatureEnabled()) return false;
    if (!x::runtime::il2cpp::Ensure()) return false;

    if (!gItemKlass) {
        gItemKlass = FindClass("", kItemClass);
        if (!gItemKlass) gItemKlass = FindClass("Msc.UI", "UIWorldMapItem");
    }
    if (!gItemKlass) return false;

    SafeRuntimeClassInit(gItemKlass);
    EnsureSpotFieldOffsets(gItemKlass);

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (!gMiUpdate) {
        using x::runtime::il2cpp_method::ResolvePath;
        constexpr MethodShape kUv{8, TypeKind::Void, false, true, {TypeKind::Ptr, TypeKind::Ptr}};
        ResolvePath pUv{};
        gMiUpdate =
            ResolveMi(gItemKlass, kRvaUpdateView, kUv, "UpdateView", kHashUpdateView, &pUv);
        static bool sUvLogged = false;
        if (!sUvLogged) {
            sUvLogged = true;
            x::runtime::LogI("WorldMapTravel", "UpdateView path=%s",
                             pUv != ResolvePath::Miss ? "meta" : "fallback");
        }
    }
    if (!gMiUpdate) {
        x::runtime::LogW("WorldMapTravel", "UpdateView MethodInfo 未找到 klass=%p", gItemKlass);
        x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "UV MI miss");
        return false;
    }

    void* origUv = nullptr;
    if (!PatchMethodInfo(gMiUpdate, reinterpret_cast<void*>(&Hook_UpdateView), &origUv)) {
        x::runtime::LogW("WorldMapTravel", "UpdateView MethodInfo 换桩失败");
        x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "UV patch fail");
        return false;
    }
    gOrigUpdate = reinterpret_cast<FnUpdateView>(origUv);

    // OnPointerDown：原生 abs jmp（BIN：纯 MI 换桩收不到点击）
    void* downNative = AtRva<void*>(kRvaOnPointerDown);
    if (!InstallAbsHook(&gDownAbs, downNative, reinterpret_cast<void*>(&Hook_OnPointerDown),
                        kDownSteal)) {
        RestoreMethodInfo(gMiUpdate, origUv);
        gOrigUpdate = nullptr;
        x::runtime::LogW("WorldMapTravel", "OnPointerDown abs hook 失败 target=%p", downNative);
        x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "Down abs fail");
        return false;
    }
    gOrigDown = reinterpret_cast<FnOnPointerDown>(gDownAbs.trampoline);

    // 顺带把 MI.methodPointer 也指到 Hook（双保险；CallOrig 走 trampoline）
    if (!gMiDown) {
        constexpr MethodShape kDn{1, TypeKind::Void, true, true, {TypeKind::Ptr}};
        gMiDown = ResolveMi(gItemKlass, kRvaOnPointerDown, kDn, "OnPointerDown", nullptr);
    }
    if (gMiDown) {
        void* ignore = nullptr;
        (void)PatchMethodInfo(gMiDown, reinterpret_cast<void*>(&Hook_OnPointerDown), &ignore);
    }

    gInstalled.store(true);
    x::runtime::LogI("WorldMapTravel",
                     "init[经典版]：UpdateView(MI)+OnPointerDown(abs) 已接管；"
                     "双击 Spot → YesNo → RequestGoto");
    x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                 "UV+DownAbs");
    return true;
}

void Uninstall() {
    if (!gInstalled.exchange(false)) return;
    RemoveAbsHook(&gDownAbs);
    if (gMiDown) {
        // abs 已还原 .text；MI 指回原生 RVA
        void* native = AtRva<void*>(kRvaOnPointerDown);
        RestoreMethodInfo(gMiDown, native);
    }
    if (gMiUpdate && gOrigUpdate) RestoreMethodInfo(gMiUpdate, reinterpret_cast<void*>(gOrigUpdate));
    gOrigDown = nullptr;
    gOrigUpdate = nullptr;
    gMiDown = nullptr;
    gMiUpdate = nullptr;
    {
        std::lock_guard<std::mutex> lock(gMu);
        gSpotByItem.clear();
        gLastClickMs.clear();
    }
}

DWORD WINAPI Worker(LPVOID) {
    while (!gStop.load()) {
        if (!gInstalled.load()) {
            const DWORD now = GetTickCount();
            if (now - gLastInstallTry >= kInstallRetryMs) {
                gLastInstallTry = now;
                (void)TryInstall();
            }
        }
        Sleep(200);
    }
    Uninstall();
    return 0;
}

}  // namespace

void Shutdown() {
    gStop.store(true);
    HANDLE th = gWorker.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
    Uninstall();
}

void Init() {
    Shutdown();
    if (!travel::IsFeatureEnabled()) {
        x::runtime::LogI("WorldMapTravel", "disabled：travel feature 已禁用");
        return;
    }
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    gWorker.store(th);
    x::runtime::LogI("WorldMapTravel", "worker start（等待 IL2CPP 后挂钩 UIWorldMapItem）");
}

}  // namespace x::features::worldmap_marker_travel
