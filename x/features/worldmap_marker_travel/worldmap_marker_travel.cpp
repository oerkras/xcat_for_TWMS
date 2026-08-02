// Classic TWMS worldmap_marker_travel
//
// 对照枫星：世界地图 Spot 双击 → 确认框 → travel::RequestGoto。
// 实现差异（Il2Cpp / 非 Lua）：
//   - UIWorldMapItem 无原生 OnDoubleClick；OnPointerDown 用 clickCount / 系统双击间隔自检。
//   - UpdateView 缓存 Spot 的 MapNo[0] + 地图名，双击时优先用图号 goto。
//   - 确认：UIUtilDialog.YesNo + 原生 System.Action（对照枫星 TextConfirm）。
//   - 瞬移石开的是 UIMapTransferDialog，不是 UIWorldMap → 无需石头/非石头门控。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "worldmap_marker_travel.h"

#include "../travel/travel.h"
#include "../ports/travel_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
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
    "ace26d8d8dcc86fd0082d56c9abb6ccac4108d3892d83a82e700b9ef50de7a1";
constexpr uint32_t kRvaUpdateView = 0x7E1230;  // remapped 2026-08-03
constexpr uint32_t kRvaOnPointerDown = 0x7E3C10;  // remapped 2026-08-03

// UIUtilDialog（非 Ex）：YesNo(string,Action,Action,…) / Notice(string,string,bool…)
constexpr char kUtilDialogClass[] =
    "d7a377d5087b2ea6eac18227d464cb82691fc08b8b0868e5058d730fca53f58";
constexpr uint32_t kRvaYesNo = 0x7445A0;  // remapped 2026-08-03
constexpr uint32_t kRvaNotice = 0x748510;  // remapped 2026-08-03

constexpr char kHashUpdateView[] =
    "b52158128620a4129cf6d98bc2ffbdbe562c1f3cc0bce6f02939e0b979ff666";
constexpr char kHashYesNo[] =
    "fa01b574a0f590c8dbbee52543fcbdaa8d3fcaa975d131c2ff4759dcabf2c05";
constexpr char kHashNotice[] =
    "f4a508d0559f8ee2cbdb5c9b57b2d95cdf2db6210f13786890afa360156fda0";

// UIWorldMapItem 字段（当前 GA：UpdateView 实机写回；cms dump 的 0x68/70/78 已偏移）
//   UpdateView: [+0x70]=mapDesc(r9), [+0x78]=mapName(r8), [+0x80]=street(rdx), [+0x88]=arg_30(int)
constexpr size_t kOffMapDesc = 0x70;
constexpr size_t kOffMapName = 0x78;
constexpr size_t kOffStreetName = 0x80;
constexpr size_t kOffCachedMapId = 0x88;

// WorldMapData.MapListData
constexpr size_t kOffMapNoList = 0x20;  // List<int> MapNo
constexpr size_t kOffMapTitle = 0x28;   // string Title
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;
constexpr size_t kOffArrayFirst = 0x20;  // Il2CppArray first element

// 双击：优先读 PointerEventData.clickCount@+0x178（Unity 已按系统双击间隔计数）；
// 自检窗口兜底用 GetDoubleClickTime()（常见 400~500ms），旧硬编码 200ms 过紧导致偶发「点了没反应」。
constexpr size_t kOffPointerClickCount = 0x178;
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

using FnUpdateView = void (*)(void* self, void* street, void* mapName, void* mapDesc,
                              void* mapListData, int32_t mapId, int32_t* outPosType,
                              void* outCurPos, const void* method);
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

using FnFieldFromName = void* (*)(void* klass, const char* name);
using FnFieldGetOffset = size_t (*)(void* field);
FnFieldFromName gFieldFromName = nullptr;
FnFieldGetOffset gFieldGetOffset = nullptr;
size_t gOffMethodPtr = 0;
size_t gOffInvokeImpl = 0;
size_t gOffExtraArg = 0;
size_t gOffMethodCode = 0;
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
    void* list = ReadPtr(mapListData, kOffMapNoList);
    if (!LooksLikeHeapPtr(list)) return 0;
    const int size = ReadI32(list, kOffListSize);
    if (size <= 0 || size > 64) return 0;
    void* items = ReadPtr(list, kOffListItems);
    if (!LooksLikeHeapPtr(items)) return 0;
    const int v = ReadI32(items, kOffArrayFirst);
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
        if (ReadIl2CppString(ReadPtr(mapListData, kOffMapTitle), tmp, sizeof(tmp)) && tmp[0] &&
            !LooksLikeMapDesc(tmp)) {
            strncpy_s(out, outSz, tmp, _TRUNCATE);
            return;
        }
    }
    if (self && ReadIl2CppString(ReadPtr(self, kOffMapName), tmp, sizeof(tmp)) && tmp[0] &&
        !LooksLikeMapDesc(tmp)) {
        strncpy_s(out, outSz, tmp, _TRUNCATE);
        return;
    }
    if (ReadIl2CppString(mapNameStr, tmp, sizeof(tmp)) && tmp[0]) {
        strncpy_s(out, outSz, tmp, _TRUNCATE);
        return;
    }
    if (self && ReadIl2CppString(ReadPtr(self, kOffMapName), tmp, sizeof(tmp)) && tmp[0]) {
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
                          const char* plain, const char* hash) {
    if (plain) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plain, shape.arity)) return mi;
    }
    if (hash) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hash, shape.arity)) return mi;
    }
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("WorldMapTravel", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plain ? plain : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, rva);
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

void CallOrigDown(void* self, void* eventData, const void* method) {
    FnOnPointerDown orig = gOrigDown;
    if (!orig) return;
    __try {
        orig(self, eventData, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void CallOrigUpdate(void* self, void* street, void* mapName, void* mapDesc, void* mapListData,
                    int32_t mapId, int32_t* outPosType, void* outCurPos, const void* method) {
    FnUpdateView orig = gOrigUpdate;
    if (!orig) return;
    __try {
        orig(self, street, mapName, mapDesc, mapListData, mapId, outPosType, outCurPos, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ShowTravelNotice(const char* msgUtf8);
bool ShowTravelConfirm(const char* target, const char* label, int hops = -2);

void Hook_UpdateView(void* self, void* street, void* mapName, void* mapDesc, void* mapListData,
                     int32_t mapId, int32_t* outPosType, void* outCurPos, const void* method) {
    (void)street;
    (void)mapDesc;
    // 先写回字段，再采证（0x78=名 / 0x70=简介 / 0x88=缓存图号）
    CallOrigUpdate(self, street, mapName, mapDesc, mapListData, mapId, outPosType, outCurPos, method);

    const int spotMap = PickMapId(mapId, mapListData, self);
    char label[160]{};
    PickLabel(mapName, mapListData, self, label, sizeof(label));
    // 若仍无短名，保留 desc 供 FireGoto 用 map_names.keyByDesc 反查
    if (!label[0]) {
        if (!ReadIl2CppString(mapDesc, label, sizeof(label)))
            (void)ReadIl2CppString(ReadPtr(self, kOffMapDesc), label, sizeof(label));
    }
    CacheSpot(self, spotMap, label);
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
            (void)ReadIl2CppString(ReadPtr(self, kOffMapDesc), nameBuf, sizeof(nameBuf));
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
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.runtimeClassInit || !klass) return;
    __try {
        e.runtimeClassInit(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WritePtrField(void* obj, size_t off, void* v) {
    if (!obj || !off) return;
    __try {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ResolveDelegateOffsets() {
    if (gDelegateOffOk) return true;
    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    if (!ga) return false;
    if (!gFieldFromName)
        gFieldFromName =
            reinterpret_cast<FnFieldFromName>(GetProcAddress(ga, "il2cpp_class_get_field_from_name"));
    if (!gFieldGetOffset)
        gFieldGetOffset =
            reinterpret_cast<FnFieldGetOffset>(GetProcAddress(ga, "il2cpp_field_get_offset"));
    void* delKlass = FindClass("System", "Delegate");
    if (!delKlass || !gFieldFromName || !gFieldGetOffset) return false;
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
    __try {
        if (!yes) yes = e.objectNew(gActionKlass);
        if (!no) no = e.objectNew(gActionKlass);
        if (!ok) ok = e.objectNew(gActionKlass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
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
    gMiYesNo = ResolveMi(gUtilDlgKlass, kRvaYesNo, kYn, "YesNo", kHashYesNo);
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
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.stringNew || !msgUtf8) return false;
    __try {
        *outMsg = e.stringNew(msgUtf8);
        *outSnd = e.stringNew("");
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
        n = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(eventData) + kOffPointerClickCount);
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
        FireGotoFromItem(self);
        return;
    }
    // BIN 探针：证明挂钩在收点击（限频，避免刷屏）
    static DWORD s_lastProbe = 0;
    const DWORD now = GetTickCount();
    if (now - s_lastProbe >= kClickProbeLogMs) {
        s_lastProbe = now;
        x::runtime::LogI("WorldMapTravel", "Spot 单击 self=%p clickCount=%d winMs=%u（再点一次触发）",
                         self, clickCount, DblClickWindowMs());
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

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    if (!gMiUpdate) {
        // UpdateView arity=8 — 哈希主
        constexpr MethodShape kUv{8, TypeKind::Void, false, true, {TypeKind::Ptr, TypeKind::Ptr}};
        gMiUpdate =
            ResolveMi(gItemKlass, kRvaUpdateView, kUv, "UpdateView", kHashUpdateView);
    }
    if (!gMiDown) {
        // Unity override 明文 OnPointerDown(PointerEventData)
        constexpr MethodShape kDn{1, TypeKind::Void, true, true, {TypeKind::Ptr}};
        gMiDown = ResolveMi(gItemKlass, kRvaOnPointerDown, kDn, "OnPointerDown", nullptr);
    }
    if (!gMiUpdate || !gMiDown) {
        x::runtime::LogW("WorldMapTravel", "MethodInfo 未齐 update=%p down=%p klass=%p",
                         (void*)gMiUpdate, (void*)gMiDown, gItemKlass);
        x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "MI miss");
        return false;
    }

    void* origUv = nullptr;
    void* origDown = nullptr;
    if (!PatchMethodInfo(gMiUpdate, reinterpret_cast<void*>(&Hook_UpdateView), &origUv)) {
        x::runtime::LogW("WorldMapTravel", "UpdateView MethodInfo 换桩失败");
        x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "UV patch fail");
        return false;
    }
    gOrigUpdate = reinterpret_cast<FnUpdateView>(origUv);
    if (!PatchMethodInfo(gMiDown, reinterpret_cast<void*>(&Hook_OnPointerDown), &origDown)) {
        RestoreMethodInfo(gMiUpdate, origUv);
        gOrigUpdate = nullptr;
        x::runtime::LogW("WorldMapTravel", "OnPointerDown MethodInfo 换桩失败");
        x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "Down patch fail");
        return false;
    }
    gOrigDown = reinterpret_cast<FnOnPointerDown>(origDown);
    gInstalled.store(true);
    x::runtime::LogI("WorldMapTravel",
                     "init[经典版]：UIWorldMapItem UpdateView+OnPointerDown 已接管；"
                     "双击 Spot → YesNo 确认 → RequestGoto（瞬移石走 MapTransferDialog，不互抢）");
    x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                 "UV+Down");
    return true;
}

void Uninstall() {
    if (!gInstalled.exchange(false)) return;
    if (gMiDown && gOrigDown) RestoreMethodInfo(gMiDown, reinterpret_cast<void*>(gOrigDown));
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
