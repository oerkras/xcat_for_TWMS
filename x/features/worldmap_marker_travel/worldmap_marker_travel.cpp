// Classic TWMS worldmap_marker_travel
//
// 对照枫星：世界地图 Spot 双击 → 确认框 → travel::RequestGoto。
// 实现差异（Il2Cpp / 非 Lua）：
//   - UIWorldMapItem 无原生 OnDoubleClick；OnPointerDown 用 clickCount / 系统双击间隔自检。
//   - UpdateView 缓存 Spot 的 MapNo[0] + 地图名，双击时优先用图号 goto。
//   - 确认：UIUtilDialog.YesNo + 原生 System.Action（对照枫星 TextConfirm）。
//   - Yes / 直通 goto 前先冻结遇人（含丢掉排队 hop），再 UIWorldMap.Close，再 RequestGoto。
//   - 瞬移石开的是 UIMapTransferDialog，不是 UIWorldMap → 无需石头/非石头门控。
// 防漂移：Spot/MapListData/clickCount 字段走 hash + field_get_offset；dump 常量仅 fallback。
// OnPointerDown 是 override：热路径 = ExecuteEvents 委托 method_ptr → 接口 VirtualInvokeData。
// 红线：本模块禁止改 GameAssembly .text。只改 MI / Delegate.method_ptr / klass vtable（数据面）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "worldmap_marker_travel.h"

#include "../travel/travel.h"
#include "../char_boot/char_boot.h"
#include "../encounter/encounter.h"
#include "../ports/travel_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/anchor_lamps.h"
#include "../../runtime/main_thread_pump.h"

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
    "e61f0e6268c0e5298cbb6c3f1a09d02c718bb2e9aee21c3230ca738e2966ebb";
constexpr uint32_t kRvaUpdateView = 0x7F4BA0;  // remounted 2026-08-06
constexpr uint32_t kRvaOnPointerDown = 0x7F7480;  // remounted 2026-08-06
// 世界地图标题点（TypeDef 636）：有 Title/坐标，无 UIWorldMapItem.UpdateView。
constexpr char kLabelClass[] =
    "d9eeaaf1b86183a9e3675291c228bbcdefd4a607970ed719cfea27620a99299";
constexpr uint32_t kRvaLabelBind = 0x7F5840;
constexpr uint32_t kRvaLabelClick = 0x7F76B0;
constexpr char kHashLabelBind[] =
    "e42d4b72f9e7d04dde8327688a43b3e4b2465bd831c7622960627fa78599efa";
// UIWorldMap / WorldMap：空 Spot 兜底只读列表，装不上不得卸主功能。
constexpr char kUiWorldMapClass[] =
    "f47394f128c6b0a64c4b9a1a92c8721871772d751dae2e123c5834c46a066ee";
constexpr char kWorldMapDataClass[] =
    "b8701472df0726cab462998013af1995491ff5b3f683bd4f3559e10f3880120";
constexpr char kLabelDataClass[] =
    "b8701472df0726cab462998013af1995491ff5b3f683bd4f3559e10f3880120."
    "a897d471d736f142a9fc98bbfb94b0c90d38dca7954d5f53af67b80c44c4dd2";
constexpr char kLabelDataClassSlash[] =
    "b8701472df0726cab462998013af1995491ff5b3f683bd4f3559e10f3880120/"
    "a897d471d736f142a9fc98bbfb94b0c90d38dca7954d5f53af67b80c44c4dd2";
constexpr char kHashWmItems[] =
    "c70e310f57f0a4e1b2acd29877752c55a97c82007a7800789819cd065562f23";
constexpr char kHashWmLabels[] =
    "f39dc4a01ce57a8080498ab20e064f9aa32b3fb8f68b6983094178811653faf";
constexpr char kHashWmWorldMap[] =
    "f763d59465fbeb7d6885d8b881d666477c63ddcae57f8955c8cce24381280ff";
constexpr char kHashLabelDataList[] =
    "f554bb6dab52b5057da954d39eaa50f81bf6bde57dda2a17281cb5a213fe18f";
constexpr char kHashLabelTitle[] =
    "c8c655dde137b7e848142476e869437ab0f90d59fa00c165b5d63af878507ee";
constexpr char kHashLabelIdx[] =
    "aaa9a90a0654ceb0f7b8a3b909dd8c4d95b031ce70b47a8a1b5b1e1fd9c558d";
constexpr char kHashLabelIdxU[] =
    "a3ab907dbaa161d2940235ca0231a827759cace9347d03d180c28ddb4bae1d6";
constexpr size_t kFbWmItems = 0xE0;
constexpr size_t kFbWmLabels = 0xE8;
constexpr size_t kFbWmWorldMap = 0x110;
constexpr size_t kFbLabelDataList = 0x30;
constexpr size_t kFbLabelTitle = 0x18;
constexpr size_t kFbLabelIdx = 0x20;
constexpr size_t kFbLabelIdxU = 0x38;
// ExecuteEvents.Execute(IPointerDownHandler, BaseEventData) — script.json Address
constexpr uint32_t kRvaExecutePointerDown = 0x52C5910;  // remounted 2026-08-06
constexpr size_t kFbSPointerDownHandler = 0x18;         // ExecuteEvents static field

// UIUtilDialog（非 Ex）：YesNo(string,Action,Action,…) / Notice(string,string,bool…)
constexpr char kUtilDialogClass[] =
    "cabf3fe9cc1437a22ff14cae558ff4ccdc75c0b90a22311eaa71c8921615d15";
constexpr uint32_t kRvaYesNo = 0x757610;  // remounted 2026-08-06
constexpr uint32_t kRvaNotice = 0x75B6A0;  // remounted 2026-08-06

constexpr char kHashUpdateView[] =
    "f05432a12325737d40d36691aa0d35df3deaadbf07ca894dc95fc4f99308ee8";
constexpr char kHashYesNo[] =
    "c4962910e9f4043e5b3a4bc923e89bb79540b5c4a921682d3466a6734db060a";
constexpr char kHashNotice[] =
    "c3384e797915ad11075b32ca1e27cd9502a2fd81c28ae8b85e5ee651f8abc25";

// dump 验证 fallback（remount 2026-08-06；UpdateView 写回 +0x70/78/80；mapId@0x88）
constexpr size_t kFbMapDesc = 0x70;
constexpr size_t kFbMapName = 0x78;
constexpr size_t kFbStreetName = 0x80;
constexpr size_t kFbCachedMapId = 0x88;
constexpr size_t kFbMapNoList = 0x20;   // List<int> MapNo
constexpr size_t kFbMapTitle = 0x28;    // string Title
constexpr size_t kFbMapListDesc = 0x30; // string Desc（MapListData）
constexpr size_t kFbListItems = 0x10;   // List._items（BCL，通常不漂）
constexpr size_t kFbListSize = 0x18;    // List._size
constexpr size_t kFbArrayFirst = 0x20;  // Il2CppArray first element
constexpr size_t kFbPointerClickCount = 0x178;

// Spot 字段哈希（dump.cs TypeDefIndex 630）
constexpr char kHashMapDesc[] =
    "c8fa53c46a67e259c5c536d9a182f45953764e306abc4f311cdc706895de1b8";
constexpr char kHashMapName[] =
    "ecbab028c2d0b6e3a15260c8cbac30275262b952d3f5dfc707c591ab3553bc8";
constexpr char kHashStreetName[] =
    "fcde867ad5679dc15cfed0f862e891bb228045adcc30a275ec20c277a2189bc";
constexpr char kHashCachedMapId[] =
    "bd97f7b984a954cba86d28ad8c6873ca19c766d7278f4735aeb4c399811366a";

// MapListData 嵌套类 + 属性 backing 字段（TypeDefIndex 2183）
constexpr char kMapListDataClass[] =
    "b8701472df0726cab462998013af1995491ff5b3f683bd4f3559e10f3880120."
    "c542c3a8842e1a6349405465280c2294c332057b15371b30f9ced6e8f4d60e8";
constexpr char kMapListDataClassSlash[] =
    "b8701472df0726cab462998013af1995491ff5b3f683bd4f3559e10f3880120/"
    "c542c3a8842e1a6349405465280c2294c332057b15371b30f9ced6e8f4d60e8";
constexpr char kMapListDataNested[] =
    "c542c3a8842e1a6349405465280c2294c332057b15371b30f9ced6e8f4d60e8";
constexpr char kHashMapNoList[] =
    "<bc66c0da95ea0e1f3cca75a0a23d6ae619d5c2b5f64472ea0757f2ef1a70cba>k__BackingField";
constexpr char kHashMapTitle[] =
    "<ecf4005f1646f00b0598b5e1ebfc8aff97c2640002b4f0298b8b84537d2018e>k__BackingField";
constexpr char kHashMapListDesc[] =
    "<ef2d6a5f706665a859175b783722d260028780f06adb0a5d892c440e5c43b71>k__BackingField";
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
using FnExecutePointerDown = void (*)(void* handler, void* eventData, const void* method);
using FnLabelBind = void (*)(void* self, void* data, void* action, const void* method);
using FnLabelClick = void (*)(void* self, void* eventData, const void* method);
using FnWmEnable = void (*)(void* self, const void* method);
using FnWmClose = void (*)(void* self, const void* method);
void Hook_OnPointerDown(void* self, void* eventData, const void* method);
void Hook_ExecutePointerDown(void* handler, void* eventData, const void* method);
void Hook_LabelBind(void* self, void* data, void* action, const void* method);
void Hook_LabelClick(void* self, void* eventData, const void* method);
void Hook_WmOnEnable(void* self, const void* method);
void Hook_WmOnDisable(void* self, const void* method);

struct SpotInfo {
    int mapId = 0;
    char label[160]{};  // 短名优先；也可能暂存简介供反查
    void* mapListData = nullptr;
};

std::mutex gMu;
std::unordered_map<void*, SpotInfo> gSpotByItem;
std::unordered_map<void*, void*> gItemByGo;  // GameObject* → UIWorldMapItem / label widget
std::unordered_map<void*, DWORD> gLastClickMs;

std::atomic<bool> gInstalled{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};

void* gItemKlass = nullptr;
void* gLabelKlass = nullptr;
void* gUiWmKlass = nullptr;
void* gUtilDlgKlass = nullptr;
void* gActionKlass = nullptr;
void* gWorldMapUi = nullptr;  // 当前打开的 UIWorldMap；OnEnable 记录
MethodInfoHead* gMiUpdate = nullptr;
MethodInfoHead* gMiDown = nullptr;
MethodInfoHead* gMiYesNo = nullptr;
MethodInfoHead* gMiNotice = nullptr;
MethodInfoHead* gMiLabelBind = nullptr;
MethodInfoHead* gMiLabelClick = nullptr;
MethodInfoHead* gMiGetGameObject = nullptr;
MethodInfoHead* gMiWmEnable = nullptr;
MethodInfoHead* gMiWmDisable = nullptr;
MethodInfoHead* gMiWmClose = nullptr;
FnUpdateView gOrigUpdate = nullptr;
FnOnPointerDown gOrigDown = nullptr;
FnExecutePointerDown gOrigExecuteDown = nullptr;
FnLabelBind gOrigLabelBind = nullptr;
FnLabelClick gOrigLabelClick = nullptr;
FnWmEnable gOrigWmEnable = nullptr;
FnWmEnable gOrigWmDisable = nullptr;
DWORD gLastInstallTry = 0;

// 数据面钩：vtable 槽 + ExecuteEvents 委托；禁止 abs/.text。
constexpr size_t kVirtInvokeStride = 16;
constexpr size_t kVtableScanLo = 0x80;
constexpr size_t kVtableScanHi = 0xC00;
constexpr int kDownSlotCap = 8;
void** gDownSlots[kDownSlotCap]{};
size_t gDownSlotOffs[kDownSlotCap]{};
int gDownSlotCount = 0;
void* gDownNativeOrig = nullptr;  // Spot OnPointerDown 原生（vtable CallOrig）
MethodInfoHead* gMiExecuteDown = nullptr;
void* gPointerDownDelegate = nullptr;
void* gDelegateMpSaved = nullptr;
void* gDelegateInvSaved = nullptr;
char gDownPath[96]{};

// 标题点 / UIWorldMap OnEnable：与 Spot Down 槽隔离，卸主功能时一并还原。
constexpr int kAuxSlotCap = 8;
void** gAuxSlots[kAuxSlotCap]{};
void* gAuxOrigs[kAuxSlotCap]{};
int gAuxSlotCount = 0;

using FnFieldFromName = void* (*)(void* klass, const char* name);
using FnFieldGetOffset = size_t (*)(void* field);
FnFieldFromName gFieldFromName = nullptr;
FnFieldGetOffset gFieldGetOffset = nullptr;
size_t gOffMethodPtr = 0;
size_t gOffInvokeImpl = 0;
size_t gOffExtraArg = 0;
size_t gOffMethodCode = 0;
bool gDelegateOffOk = false;

void WritePtrField(void* obj, size_t off, void* v);
bool ResolveDelegateOffsets();
void CacheSpot(void* item, int mapId, const char* label, void* mapListData = nullptr);
bool LookupSpot(void* item, SpotInfo* out);
bool IsWorldMapSpotItem(void* obj);
bool IsWorldMapLabelItem(void* obj);
void* TryCompGameObject(void* comp);
void EnsureWmFieldOffsets(void* uiKlass);
void CloseWorldMapUi();
MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr);

struct SpotFieldOff {
    size_t mapDesc = kFbMapDesc;
    size_t mapName = kFbMapName;
    size_t streetName = kFbStreetName;
    size_t cachedMapId = kFbCachedMapId;
    size_t mapNoList = kFbMapNoList;
    size_t mapTitle = kFbMapTitle;
    size_t mapListDesc = kFbMapListDesc;
    size_t listItems = kFbListItems;
    size_t listSize = kFbListSize;
    size_t arrayFirst = kFbArrayFirst;
    size_t pointerClickCount = kFbPointerClickCount;
    bool tried = false;
    const char* path = "fallback";  // meta | meta-partial | fallback
};
SpotFieldOff gSpotOff{};

struct WmFieldOff {
    size_t items = kFbWmItems;
    size_t labels = kFbWmLabels;
    size_t worldMap = kFbWmWorldMap;
    size_t labelDataList = kFbLabelDataList;
    size_t labelTitle = kFbLabelTitle;
    size_t labelIdx = kFbLabelIdx;
    size_t labelIdxU = kFbLabelIdxU;
    bool tried = false;
    const char* path = "fallback";
};
WmFieldOff gWmOff{};

void* gYesAction = nullptr;
void* gNoAction = nullptr;
void* gOkAction = nullptr;
uintptr_t gYesHandle = 0;
uintptr_t gNoHandle = 0;
uintptr_t gOkHandle = 0;

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

void* TryCompGameObject(void* comp) {
    if (!LooksLikeHeapPtr(comp)) return nullptr;
    if (!gMiGetGameObject) {
        void* ck = FindClass("UnityEngine", "Component");
        if (!ck) return nullptr;
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kGo{0, TypeKind::Ptr, true, true};
        gMiGetGameObject =
            ResolveMi(ck, x::runtime::il2cpp::kRvaCompGetGo, kGo, "get_gameObject", nullptr);
        if (!gMiGetGameObject) return nullptr;
    }
    auto fn = x::runtime::il2cpp::AtRva<x::runtime::il2cpp::FnCompGo>(x::runtime::il2cpp::kRvaCompGetGo);
    if (!fn) return nullptr;
    void* go = nullptr;
    __try {
        go = fn(comp, gMiGetGameObject);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        go = nullptr;
    }
    return LooksLikeHeapPtr(go) ? go : nullptr;
}

void CacheSpot(void* item, int mapId, const char* label, void* mapListData) {
    if (!item) return;
    void* go = TryCompGameObject(item);
    std::lock_guard<std::mutex> lock(gMu);
    if (gSpotByItem.size() >= kSpotCacheMax && !gSpotByItem.count(item)) {
        gSpotByItem.clear();
        gItemByGo.clear();
        gLastClickMs.clear();
    }
    SpotInfo& s = gSpotByItem[item];
    if (mapId > 0) s.mapId = mapId;
    if (label && label[0]) {
        strncpy_s(s.label, label, _TRUNCATE);
    }
    if (LooksLikeHeapPtr(mapListData)) s.mapListData = mapListData;
    if (LooksLikeHeapPtr(go)) gItemByGo[go] = item;
}

bool LookupSpot(void* item, SpotInfo* out) {
    if (!item || !out) return false;
    std::lock_guard<std::mutex> lock(gMu);
    auto it = gSpotByItem.find(item);
    if (it == gSpotByItem.end()) return false;
    *out = it->second;
    return out->mapId > 0 || out->label[0] != '\0' || out->mapListData != nullptr;
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

MethodInfoHead* FindDeclaredMethod(void* klass, const char* name) {
    if (!klass || !name) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods) return nullptr;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(klass, &iter);
            if (!raw) break;
            const char* nm = e.methodGetName ? e.methodGetName(raw) : nullptr;
            if (nm && strcmp(nm, name) == 0) {
                auto* mi = reinterpret_cast<MethodInfoHead*>(raw);
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
                          x::runtime::il2cpp_method::ResolvePath* outPath) {
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
    void* vmp = nullptr;
    __try {
        orig = mi->methodPointer;
        vmp = mi->virtualMethodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        mi->methodPointer = hook;
        // override / 接口派发常读 virtualMethodPointer；只要仍指向原生就一并换。
        if (vmp == orig || vmp == nullptr) mi->virtualMethodPointer = hook;
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

bool PatchVtableMethodPtr(void** slot, void* hook, void** outOrig) {
    if (!slot || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) {
        if (orig == hook && gDownNativeOrig) {
            *outOrig = gDownNativeOrig;
            return true;
        }
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        *slot = hook;
        *outOrig = orig;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    return ok;
}

void RestoreVtableMethodPtr(void** slot, void* orig) {
    if (!slot || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    __try {
        *slot = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
}

uint32_t PtrRva(void* p) {
    HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
    if (!ga || !p) return 0;
    const auto a = reinterpret_cast<uintptr_t>(p);
    const auto b = reinterpret_cast<uintptr_t>(ga);
    if (a < b) return 0;
    const auto d = a - b;
    return d > 0x7FFFFFFFull ? 0 : static_cast<uint32_t>(d);
}

// 扫 VirtualInvokeData{methodPtr, MethodInfo*}；认 MI 对上 / ptr / RVA（含 adjustor 槽）。
int FindDownVtableSlots(void* klass, MethodInfoHead* mi, void* wantPtr, uint32_t wantRva,
                        void*** outSlots, size_t* outOffs, int cap, const char** outPath) {
    if (!klass || !outSlots || !outOffs || cap <= 0) return 0;
    int n = 0;
    int bestScore = 0;
    auto push = [&](size_t off, int score) {
        void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off);
        for (int i = 0; i < n; ++i) {
            if (outSlots[i] == slot) return;
        }
        if (n >= cap) return;
        outSlots[n] = slot;
        outOffs[n] = off;
        ++n;
        if (score > bestScore) bestScore = score;
    };
    for (size_t off = kVtableScanLo; off + kVirtInvokeStride <= kVtableScanHi; off += 8) {
        void* p0 = nullptr;
        void* p1 = nullptr;
        __try {
            p0 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off);
            p1 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off + 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        const bool miHit = mi && p1 == mi;
        const bool ptrHit = wantPtr && p0 == wantPtr;
        const bool rvaHit = wantRva && PtrRva(p0) == wantRva;
        if (!miHit && !ptrHit && !rvaHit) continue;
        if (!LooksLikeHeapPtr(p1) && !miHit) continue;
        if (miHit && (ptrHit || rvaHit))
            push(off, 3);
        else if (miHit)
            push(off, 2);  // adjustor：methodPtr≠MI.mp，但仍是本方法槽
        else
            push(off, 1);
    }
    if (outPath) {
        if (n == 0)
            *outPath = "miss";
        else if (bestScore >= 3)
            *outPath = "scan-pair";
        else if (bestScore >= 2)
            *outPath = "scan-mi";
        else
            *outPath = "scan-ptr";
    }
    return n;
}

int FindDownVtableSlotsOnHierarchy(void* klass, MethodInfoHead* mi, void* wantPtr, uint32_t wantRva,
                                   void*** outSlots, size_t* outOffs, int cap, const char** outPath) {
    int n = 0;
    const char* path = "miss";
    const auto& e = x::runtime::il2cpp::Get();
    void* cur = klass;
    for (int depth = 0; cur && depth < 8; ++depth) {
        const char* sub = "miss";
        void** slots[kDownSlotCap]{};
        size_t offs[kDownSlotCap]{};
        const int got =
            FindDownVtableSlots(cur, mi, wantPtr, wantRva, slots, offs, kDownSlotCap, &sub);
        for (int i = 0; i < got && n < cap; ++i) {
            bool dup = false;
            for (int j = 0; j < n; ++j) {
                if (outSlots[j] == slots[i]) {
                    dup = true;
                    break;
                }
            }
            if (dup) continue;
            outSlots[n] = slots[i];
            outOffs[n] = offs[i];
            ++n;
        }
        if (n > 0 && strcmp(sub, "miss") != 0) path = sub;
        if (!e.classParent) break;
        void* parent = nullptr;
        __try {
            parent = e.classParent(cur);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        cur = parent;
    }
    if (outPath) *outPath = n ? path : "miss";
    return n;
}

void CallOrigDown(void* self, void* eventData, const void* method) {
    FnOnPointerDown orig = gOrigDown;
    if (!orig) return;
    __try {
        orig(self, eventData, method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ClearDownHooks() {
    void* itemHook = reinterpret_cast<void*>(&Hook_OnPointerDown);
    for (int i = 0; i < gDownSlotCount; ++i) {
        if (!gDownSlots[i] || !gDownNativeOrig) continue;
        void* cur = nullptr;
        __try {
            cur = *gDownSlots[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cur = nullptr;
        }
        if (cur == itemHook) RestoreVtableMethodPtr(gDownSlots[i], gDownNativeOrig);
        gDownSlots[i] = nullptr;
        gDownSlotOffs[i] = 0;
    }
    gDownSlotCount = 0;

    if (gPointerDownDelegate && ResolveDelegateOffsets()) {
        if (gDelegateMpSaved) WritePtrField(gPointerDownDelegate, gOffMethodPtr, gDelegateMpSaved);
        if (gDelegateInvSaved && gOffInvokeImpl)
            WritePtrField(gPointerDownDelegate, gOffInvokeImpl, gDelegateInvSaved);
    }
    gPointerDownDelegate = nullptr;
    gDelegateMpSaved = nullptr;
    gDelegateInvSaved = nullptr;

    if (gMiExecuteDown && gOrigExecuteDown)
        RestoreMethodInfo(gMiExecuteDown, reinterpret_cast<void*>(gOrigExecuteDown));
    gMiExecuteDown = nullptr;
    gOrigExecuteDown = nullptr;

    if (gMiDown && gDownNativeOrig) RestoreMethodInfo(gMiDown, gDownNativeOrig);
    gMiDown = nullptr;
    gOrigDown = nullptr;
    gDownNativeOrig = nullptr;
    gDownPath[0] = '\0';
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
    CacheSpot(self, spotMap, label, mapListData);

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

bool TryAcceptMapId(int id, int* mapId, char* key, size_t keySz, const char* via) {
    if (id <= 0 || !mapId || !key) return false;
    const auto& pack = xcat::GetSharedMapNames(x::runtime::GetBinDir());
    if (!xcat::MapNamesHasId(pack, id)) return false;
    *mapId = id;
    snprintf(key, keySz, "%d", id);
    x::runtime::LogI("WorldMapTravel", "empty-spot resolve via=%s mapId=%d", via ? via : "?", id);
    return true;
}

bool TryAcceptExactName(const char* name, int* mapId, char* key, size_t keySz, char* nameBuf,
                        size_t nameSz, const char* via) {
    if (!name || !name[0] || !mapId || !key) return false;
    const auto& pack = xcat::GetSharedMapNames(x::runtime::GetBinDir());
    const std::string resolved = xcat::MapNamesResolveExact(pack, name);
    if (resolved.empty()) return false;
    int id = 0;
    try {
        id = std::stoi(resolved);
    } catch (...) {
        id = 0;
    }
    if (id <= 0) return false;
    *mapId = id;
    snprintf(key, keySz, "%d", id);
    if (nameBuf && nameSz && !LooksLikeMapDesc(name)) strncpy_s(nameBuf, nameSz, name, _TRUNCATE);
    x::runtime::LogI("WorldMapTravel", "empty-spot resolve via=%s name=%s mapId=%d",
                     via ? via : "?", name, id);
    return true;
}

void FillFromMapListData(void* mld, int* mapId, char* nameBuf, size_t nameSz) {
    if (!LooksLikeHeapPtr(mld) || !mapId) return;
    if (*mapId <= 0) {
        const int id = FirstMapNo(mld);
        if (id > 0) *mapId = id;
    }
    if (!nameBuf || !nameSz || nameBuf[0]) return;
    char tmp[160]{};
    if (ReadIl2CppString(ReadPtr(mld, gSpotOff.mapTitle), tmp, sizeof(tmp)) && tmp[0]) {
        strncpy_s(nameBuf, nameSz, tmp, _TRUNCATE);
        return;
    }
    (void)ReadIl2CppString(ReadPtr(mld, gSpotOff.mapListDesc), nameBuf, static_cast<int>(nameSz));
}

void CacheLabelData(void* widget, void* data) {
    if (!LooksLikeHeapPtr(widget) || !LooksLikeHeapPtr(data)) return;
    const int idA = ReadI32(data, 0x20);
    const int idB = ReadI32(data, 0x38);
    char title[160]{};
    if (!ReadIl2CppString(ReadPtr(data, 0x18), title, sizeof(title))) title[0] = 0;
    if (!title[0]) (void)ReadIl2CppString(ReadPtr(data, 0x28), title, sizeof(title));
    if (!title[0]) (void)ReadIl2CppString(ReadPtr(data, gWmOff.labelTitle), title, sizeof(title));
    if (!title[0]) (void)ReadIl2CppString(ReadPtr(data, 0x30), title, sizeof(title));
    const auto& pack = xcat::GetSharedMapNames(x::runtime::GetBinDir());
    int mapId = 0;
    if (xcat::MapNamesHasId(pack, idA)) mapId = idA;
    else if (xcat::MapNamesHasId(pack, idB)) mapId = idB;
    CacheSpot(widget, mapId, title[0] ? title : nullptr, nullptr);
}

bool TryResolveFromCachedItem(void* item, int* mapId, char* key, size_t keySz, char* nameBuf,
                              size_t nameSz, const char* via) {
    if (!item) return false;
    SpotInfo spot{};
    if (!LookupSpot(item, &spot)) return false;
    FillFromMapListData(spot.mapListData, &spot.mapId, spot.label, sizeof(spot.label));
    if (spot.mapId > 0 && TryAcceptMapId(spot.mapId, mapId, key, keySz, via)) {
        if (nameBuf && nameSz && spot.label[0] && !LooksLikeMapDesc(spot.label))
            strncpy_s(nameBuf, nameSz, spot.label, _TRUNCATE);
        return true;
    }
    if (spot.label[0] &&
        TryAcceptExactName(spot.label, mapId, key, keySz, nameBuf, nameSz, via)) {
        return true;
    }
    return false;
}

bool TryResolveFromEvent(void* eventData, int* mapId, char* key, size_t keySz, char* nameBuf,
                         size_t nameSz) {
    if (!LooksLikeHeapPtr(eventData)) return false;
    void* gos[12]{};
    int n = 0;
    auto pushGo = [&](void* p) {
        if (!LooksLikeHeapPtr(p) || n >= 12) return;
        for (int i = 0; i < n; ++i)
            if (gos[i] == p) return;
        gos[n++] = p;
    };
    pushGo(ReadPtr(eventData, 0x20));  // pointerEnter
    pushGo(ReadPtr(eventData, 0x28));  // pointerPress
    pushGo(ReadPtr(eventData, 0x30));  // lastPress
    pushGo(ReadPtr(eventData, 0x38));  // rawPointerPress
    pushGo(ReadPtr(eventData, 0x48));  // pointerClick
    pushGo(ReadPtr(eventData, 0x50));  // RaycastResult.m_GameObject
    void* hovered = ReadPtr(eventData, 0x130);
    if (LooksLikeHeapPtr(hovered)) {
        const int hn = ReadI32(hovered, gSpotOff.listSize);
        void* items = ReadPtr(hovered, gSpotOff.listItems);
        if (LooksLikeHeapPtr(items) && hn > 0 && hn <= 32) {
            for (int i = 0; i < hn && n < 12; ++i) {
                pushGo(ReadPtr(items, gSpotOff.arrayFirst + static_cast<size_t>(i) * sizeof(void*)));
            }
        }
    }

    int foundId = 0;
    char foundName[160]{};
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        void* widget = nullptr;
        {
            std::lock_guard<std::mutex> lock(gMu);
            auto it = gItemByGo.find(gos[i]);
            if (it != gItemByGo.end()) widget = it->second;
        }
        if (!widget) continue;
        int id = 0;
        char kbuf[32]{};
        char nbuf[160]{};
        if (!TryResolveFromCachedItem(widget, &id, kbuf, sizeof(kbuf), nbuf, sizeof(nbuf),
                                      "hovered")) {
            continue;
        }
        if (hits == 0) {
            foundId = id;
            if (nbuf[0]) strncpy_s(foundName, nbuf, _TRUNCATE);
        } else if (foundId != id) {
            x::runtime::LogW("WorldMapTravel", "empty-spot hovered 多图号 %d vs %d，放弃兜底",
                             foundId, id);
            return false;
        }
        ++hits;
    }
    if (hits == 1) {
        if (!TryAcceptMapId(foundId, mapId, key, keySz, "hovered-unique")) return false;
        if (foundName[0] && nameBuf && nameSz)
            strncpy_s(nameBuf, nameSz, foundName, _TRUNCATE);
        return true;
    }
    return false;
}

void* ListAt(void* list, int i) {
    if (!LooksLikeHeapPtr(list) || i < 0) return nullptr;
    const int n = ReadI32(list, gSpotOff.listSize);
    if (i >= n || n > 256) return nullptr;
    void* items = ReadPtr(list, gSpotOff.listItems);
    if (!LooksLikeHeapPtr(items)) return nullptr;
    return ReadPtr(items, gSpotOff.arrayFirst + static_cast<size_t>(i) * sizeof(void*));
}

int ListCount(void* list) {
    if (!LooksLikeHeapPtr(list)) return 0;
    const int n = ReadI32(list, gSpotOff.listSize);
    return (n > 0 && n <= 256) ? n : 0;
}

void CacheFromWorldMapUi(void* ui) {
    if (!LooksLikeHeapPtr(ui)) return;
    if (gUiWmKlass) EnsureWmFieldOffsets(gUiWmKlass);
    void* labels = ReadPtr(ui, gWmOff.labels);
    void* wm = ReadPtr(ui, gWmOff.worldMap);
    void* dataList = LooksLikeHeapPtr(wm) ? ReadPtr(wm, gWmOff.labelDataList) : nullptr;
    const int nLabel = ListCount(labels);
    const int nData = ListCount(dataList);
    const int n = nLabel < nData ? nLabel : nData;
    int cached = 0;
    for (int i = 0; i < n; ++i) {
        void* widget = ListAt(labels, i);
        void* data = ListAt(dataList, i);
        if (!LooksLikeHeapPtr(widget) || !LooksLikeHeapPtr(data)) continue;
        CacheLabelData(widget, data);
        ++cached;
    }
    if (cached > 0) {
        static DWORD s_last = 0;
        const DWORD now = GetTickCount();
        if (now - s_last >= 2000) {
            s_last = now;
            x::runtime::LogI("WorldMapTravel", "empty-spot 缓存标题点 n=%d ui=%p", cached, ui);
        }
    }
}

bool TryResolveFromWorldMap(void* self, int* mapId, char* key, size_t keySz, char* nameBuf,
                            size_t nameSz) {
    void* ui = gWorldMapUi;
    if (!LooksLikeHeapPtr(ui) || !self) return false;
    CacheFromWorldMapUi(ui);
    const int fieldId = ReadI32(self, gSpotOff.cachedMapId);
    if (fieldId <= 0) return false;
    const auto& pack = xcat::GetSharedMapNames(x::runtime::GetBinDir());
    if (xcat::MapNamesHasId(pack, fieldId)) return false;

    void* wm = ReadPtr(ui, gWmOff.worldMap);
    void* dataList = LooksLikeHeapPtr(wm) ? ReadPtr(wm, gWmOff.labelDataList) : nullptr;
    const int n = ListCount(dataList);
    int foundId = 0;
    char foundName[160]{};
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        void* data = ListAt(dataList, i);
        if (!LooksLikeHeapPtr(data)) continue;
        const int idA = ReadI32(data, gWmOff.labelIdx);
        const int idB = ReadI32(data, gWmOff.labelIdxU);
        if (idA != fieldId && idB != fieldId) continue;
        char title[160]{};
        if (!ReadIl2CppString(ReadPtr(data, gWmOff.labelTitle), title, sizeof(title))) title[0] = 0;
        if (!title[0]) (void)ReadIl2CppString(ReadPtr(data, 0x18), title, sizeof(title));
        if (!title[0] || LooksLikeMapDesc(title)) continue;
        int id = 0;
        char kbuf[32]{};
        char nbuf[160]{};
        if (!TryAcceptExactName(title, &id, kbuf, sizeof(kbuf), nbuf, sizeof(nbuf), "wm-index")) {
            continue;
        }
        if (hits == 0) {
            foundId = id;
            if (nbuf[0]) strncpy_s(foundName, nbuf, _TRUNCATE);
        } else if (foundId != id) {
            x::runtime::LogW("WorldMapTravel", "empty-spot wm-index 多图号 %d vs %d fieldId=%d，放弃",
                             foundId, id, fieldId);
            return false;
        }
        ++hits;
    }
    if (hits != 1) return false;
    if (!TryAcceptMapId(foundId, mapId, key, keySz, "wm-index-unique")) return false;
    if (foundName[0] && nameBuf && nameSz) strncpy_s(nameBuf, nameSz, foundName, _TRUNCATE);
    return true;
}

void FireGotoFromItem(void* self, void* eventData = nullptr) {
    if (char_boot::IsBusy()) {
        (void)ShowTravelNotice("起号进行中");
        return;
    }
    SpotInfo spot{};
    char nameBuf[160]{};
    char key[32]{};
    int mapId = 0;
    void* mld = nullptr;

    if (LookupSpot(self, &spot)) {
        mapId = spot.mapId;
        mld = spot.mapListData;
        if (spot.label[0]) strncpy_s(nameBuf, spot.label, _TRUNCATE);
    }
    FillFromMapListData(mld, &mapId, nameBuf, sizeof(nameBuf));
    if (mapId <= 0) {
        mapId = PickMapId(0, mld, self);
    }
    if (mapId <= 0) {
        const int fieldId = ReadI32(self, gSpotOff.cachedMapId);
        const auto& pack = xcat::GetSharedMapNames(x::runtime::GetBinDir());
        if (xcat::MapNamesHasId(pack, fieldId)) mapId = fieldId;
    }
    if (!nameBuf[0] || LooksLikeMapDesc(nameBuf)) {
        char fresh[160]{};
        PickLabel(nullptr, mld, self, fresh, sizeof(fresh));
        if (fresh[0]) {
            if (!LooksLikeMapDesc(fresh) || !nameBuf[0]) strncpy_s(nameBuf, fresh, _TRUNCATE);
        } else if (!nameBuf[0]) {
            (void)ReadIl2CppString(ReadPtr(self, gSpotOff.mapDesc), nameBuf, sizeof(nameBuf));
        }
    }
    if (!nameBuf[0]) {
        (void)ReadIl2CppString(ReadPtr(self, gSpotOff.streetName), nameBuf, sizeof(nameBuf));
    }
    if (mapId > 0) snprintf(key, sizeof(key), "%d", mapId);

    // 有图号的旧路径保持 MapNamesResolveQuery（短名子串仍给目录/正常 Spot）。
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

    // 仅失败路径：精确名 / 悬停栈里唯一已缓存点 / 世界地图标题表按下标。禁止子串、禁止多命中猜图。
    if (!key[0]) {
        if (nameBuf[0]) {
            (void)TryAcceptExactName(nameBuf, &mapId, key, sizeof(key), nameBuf, sizeof(nameBuf),
                                     "self-exact");
        }
    }
    if (!key[0]) {
        (void)TryResolveFromWorldMap(self, &mapId, key, sizeof(key), nameBuf, sizeof(nameBuf));
    }
    if (!key[0]) {
        (void)TryResolveFromEvent(eventData, &mapId, key, sizeof(key), nameBuf, sizeof(nameBuf));
    }

    if (!key[0]) {
        const int fieldId = self ? ReadI32(self, gSpotOff.cachedMapId) : 0;
        x::runtime::LogW("WorldMapTravel",
                         "双击 Spot 无法解析图号 mapId=%d fieldId=%d label=%s mld=%p，跳过", mapId,
                         fieldId, nameBuf[0] ? nameBuf : "-", mld);
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
    const int want = 8;  // Spot×4 + MapList×3 + clickCount（BCL List 偏移不计入）
    size_t oDesc = kFbMapDesc, oName = kFbMapName, oStreet = kFbStreetName, oId = kFbCachedMapId;
    size_t oList = kFbMapNoList, oTitle = kFbMapTitle, oListDesc = kFbMapListDesc;
    size_t oClick = kFbPointerClickCount;

    if (FieldOffOrFb(itemKlass, kHashMapDesc, kFbMapDesc, &oDesc)) ++hits;
    if (FieldOffOrFb(itemKlass, kHashMapName, kFbMapName, &oName)) ++hits;
    if (FieldOffOrFb(itemKlass, kHashStreetName, kFbStreetName, &oStreet)) ++hits;
    if (FieldOffOrFb(itemKlass, kHashCachedMapId, kFbCachedMapId, &oId)) ++hits;
    if (FieldOffOrFb(mldKlass, kHashMapNoList, kFbMapNoList, &oList)) ++hits;
    if (FieldOffOrFb(mldKlass, kHashMapTitle, kFbMapTitle, &oTitle)) ++hits;
    if (FieldOffOrFb(mldKlass, kHashMapListDesc, kFbMapListDesc, &oListDesc)) ++hits;
    if (FieldOffOrFb(pedKlass, kHashClickCount, kFbPointerClickCount, &oClick)) ++hits;

    gSpotOff.mapDesc = oDesc;
    gSpotOff.mapName = oName;
    gSpotOff.streetName = oStreet;
    gSpotOff.cachedMapId = oId;
    gSpotOff.mapNoList = oList;
    gSpotOff.mapTitle = oTitle;
    gSpotOff.mapListDesc = oListDesc;
    gSpotOff.pointerClickCount = oClick;
    gSpotOff.listItems = x::runtime::il2cpp_container::OffListItems();
    gSpotOff.listSize = x::runtime::il2cpp_container::OffListSize();
    gSpotOff.arrayFirst = x::runtime::il2cpp_container::OffArrayData();
    gSpotOff.path = hits == want ? "meta" : (hits ? "meta-partial" : "fallback");

    x::runtime::LogI(
        "WorldMapTravel",
        "offsets path=%s hits=%d/%d desc=0x%zx name=0x%zx street=0x%zx id=0x%zx "
        "mapNo=0x%zx title=0x%zx listDesc=0x%zx click=0x%zx",
        gSpotOff.path, hits, want, gSpotOff.mapDesc, gSpotOff.mapName, gSpotOff.streetName,
        gSpotOff.cachedMapId, gSpotOff.mapNoList, gSpotOff.mapTitle, gSpotOff.mapListDesc,
        gSpotOff.pointerClickCount);
}

void* FindLabelDataKlass() {
    void* k = FindClass("", kLabelDataClass);
    if (!k) k = FindClass("", kLabelDataClassSlash);
    return k;
}

void EnsureWmFieldOffsets(void* uiKlass) {
    if (gWmOff.tried) return;
    gWmOff.tried = true;
    void* wmKlass = FindClass("", kWorldMapDataClass);
    void* ldKlass = FindLabelDataKlass();
    int hits = 0;
    const int want = 7;
    size_t oItems = kFbWmItems, oLabels = kFbWmLabels, oWm = kFbWmWorldMap;
    size_t oLd = kFbLabelDataList, oTitle = kFbLabelTitle, oIdx = kFbLabelIdx, oIdxU = kFbLabelIdxU;
    if (FieldOffOrFb(uiKlass, kHashWmItems, kFbWmItems, &oItems)) ++hits;
    if (FieldOffOrFb(uiKlass, kHashWmLabels, kFbWmLabels, &oLabels)) ++hits;
    if (FieldOffOrFb(uiKlass, kHashWmWorldMap, kFbWmWorldMap, &oWm)) ++hits;
    if (FieldOffOrFb(wmKlass, kHashLabelDataList, kFbLabelDataList, &oLd)) ++hits;
    if (FieldOffOrFb(ldKlass, kHashLabelTitle, kFbLabelTitle, &oTitle)) ++hits;
    if (FieldOffOrFb(ldKlass, kHashLabelIdx, kFbLabelIdx, &oIdx)) ++hits;
    if (FieldOffOrFb(ldKlass, kHashLabelIdxU, kFbLabelIdxU, &oIdxU)) ++hits;
    gWmOff.items = oItems;
    gWmOff.labels = oLabels;
    gWmOff.worldMap = oWm;
    gWmOff.labelDataList = oLd;
    gWmOff.labelTitle = oTitle;
    gWmOff.labelIdx = oIdx;
    gWmOff.labelIdxU = oIdxU;
    gWmOff.path = hits == want ? "meta" : (hits ? "meta-partial" : "fallback");
    x::runtime::LogI("WorldMapTravel",
                     "wm-off path=%s hits=%d/%d items=0x%zx labels=0x%zx wm=0x%zx "
                     "ldList=0x%zx title=0x%zx idx=0x%zx idxU=0x%zx",
                     gWmOff.path, hits, want, gWmOff.items, gWmOff.labels, gWmOff.worldMap,
                     gWmOff.labelDataList, gWmOff.labelTitle, gWmOff.labelIdx, gWmOff.labelIdxU);
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

bool EnsureWmCloseMi() {
    if (gMiWmClose && gMiWmClose->methodPointer) return true;
    if (!gUiWmKlass) gUiWmKlass = FindClass("", kUiWorldMapClass);
    if (!gUiWmKlass) return false;
    gMiWmClose = FindDeclaredMethod(gUiWmKlass, "Close");
    if (!gMiWmClose) gMiWmClose = FindMethodByName(gUiWmKlass, "Close", 0);
    if (!gMiWmClose) {
        const auto& e = x::runtime::il2cpp::Get();
        void* parent = nullptr;
        if (e.classParent) {
            __try {
                parent = e.classParent(gUiWmKlass);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                parent = nullptr;
            }
        }
        if (parent && parent != gUiWmKlass) gMiWmClose = FindMethodByName(parent, "Close", 0);
    }
    return gMiWmClose && gMiWmClose->methodPointer;
}

void CloseWorldMapUiNow() {
    void* ui = gWorldMapUi;
    if (!LooksLikeHeapPtr(ui)) {
        x::runtime::LogI("WorldMapTravel", "确认赶路：世界地图指针空，跳过 Close");
        return;
    }
    if (!EnsureWmCloseMi()) {
        x::runtime::LogW("WorldMapTravel", "确认赶路：UIWorldMap.Close MethodInfo 未找到 ui=%p", ui);
        return;
    }
    auto fn = reinterpret_cast<FnWmClose>(gMiWmClose->methodPointer);
    bool seh = false;
    __try {
        fn(ui, gMiWmClose);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        seh = true;
    }
    if (seh) {
        x::runtime::LogW("WorldMapTravel", "确认赶路：UIWorldMap.Close SEH ui=%p", ui);
        return;
    }
    x::runtime::LogI("WorldMapTravel", "确认赶路：已 Close 世界地图 ui=%p", ui);
}

void CloseWorldMapJob(void*) {
    CloseWorldMapUiNow();
}

void CloseWorldMapUi() {
    if (x::runtime::main_thread::IsOnPumpThread()) {
        CloseWorldMapUiNow();
        return;
    }
    if (!x::runtime::main_thread::Ensure()) {
        x::runtime::LogW("WorldMapTravel", "确认赶路：泵未就绪，跳过 Close 世界地图");
        return;
    }
    if (!x::runtime::main_thread::InvokeAndWait(&CloseWorldMapJob, nullptr, 1500,
                                                x::runtime::main_thread::JobPrio::High)) {
        x::runtime::LogW("WorldMapTravel", "确认赶路：Close 世界地图投泵失败，仍继续 goto");
    }
}

void StartGotoFromWorldMap(const char* target) {
    // Close 世界地图可能投泵等 1.5s；遇人 Confirming/Hopping 必须先冻结，否则会抢 CloseSession。
    encounter::SuspendNow("worldmap_goto");
    CloseWorldMapUi();
    travel::RequestGoto(target);
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
    if (char_boot::IsBusy()) {
        (void)ShowTravelNotice("起号进行中");
        return;
    }
    x::runtime::LogI("WorldMapTravel",
                     "确认赶路 → 冻结遇人 → Close世界地图 → RequestGoto [%s]%s%s", target,
                     label[0] ? " " : "", label[0] ? label : "");
    StartGotoFromWorldMap(target);
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
        const uintptr_t hy = e.gcHandleNew(yes, false);
        if (!hy) return false;
        gYesHandle = hy;
        gYesAction = yes;
    }
    if (!gNoHandle) {
        const uintptr_t hn = e.gcHandleNew(no, false);
        if (!hn) return false;
        gNoHandle = hn;
        gNoAction = no;
    }
    if (!gOkHandle) {
        const uintptr_t ho = e.gcHandleNew(ok, false);
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
        if (char_boot::IsBusy()) {
            (void)ShowTravelNotice("起号进行中");
            return false;
        }
        StartGotoFromWorldMap(target);
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
        if (char_boot::IsBusy()) {
            (void)ShowTravelNotice("起号进行中");
            return false;
        }
        StartGotoFromWorldMap(target);
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

bool IsWorldMapSpotItem(void* obj) {
    if (!obj || !gItemKlass) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.objectGetClass) return false;
    void* k = nullptr;
    __try {
        k = e.objectGetClass(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (int depth = 0; k && depth < 8; ++depth) {
        if (k == gItemKlass) return true;
        if (!e.classParent) break;
        void* p = nullptr;
        __try {
            p = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            p = nullptr;
        }
        k = p;
    }
    return false;
}

bool IsWorldMapLabelItem(void* obj) {
    if (!obj || !gLabelKlass) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.objectGetClass) return false;
    void* k = nullptr;
    __try {
        k = e.objectGetClass(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (int depth = 0; k && depth < 8; ++depth) {
        if (k == gLabelKlass) return true;
        if (!e.classParent) break;
        void* p = nullptr;
        __try {
            p = e.classParent(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            p = nullptr;
        }
        k = p;
    }
    return false;
}

void Hook_LabelBind(void* self, void* data, void* action, const void* method) {
    FnLabelBind orig = gOrigLabelBind;
    if (orig) {
        __try {
            orig(self, data, action, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    CacheLabelData(self, data);
}

void Hook_LabelClick(void* self, void* eventData, const void* method) {
    int clickCount = 0;
    const bool dbl = NoteClickAndIsDouble(self, eventData, &clickCount);
    FnLabelClick orig = gOrigLabelClick;
    if (orig) {
        __try {
            orig(self, eventData, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!dbl) return;
    x::runtime::LogI("WorldMapTravel", "标题点双击 self=%p clickCount=%d", self, clickCount);
    FireGotoFromItem(self, eventData);
}

void Hook_WmOnEnable(void* self, const void* method) {
    FnWmEnable orig = gOrigWmEnable;
    if (orig) {
        __try {
            orig(self, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (LooksLikeHeapPtr(self)) {
        gWorldMapUi = self;
        CacheFromWorldMapUi(self);
    }
}

void Hook_WmOnDisable(void* self, const void* method) {
    FnWmEnable orig = gOrigWmDisable;
    if (orig) {
        __try {
            orig(self, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (self && self == gWorldMapUi) gWorldMapUi = nullptr;
}

void Hook_OnPointerDown(void* self, void* eventData, const void* method) {
    // 仅 vtable 单路径时在此处理；ExecuteEvents 路径已在 Hook_ExecutePointerDown 处理。
    int clickCount = 0;
    const bool dbl = NoteClickAndIsDouble(self, eventData, &clickCount);
    CallOrigDown(self, eventData, method);
    if (dbl) {
        x::runtime::LogI("WorldMapTravel", "Spot 双击判定 self=%p clickCount=%d", self, clickCount);
        FireGotoFromItem(self, eventData);
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

void Hook_ExecutePointerDown(void* handler, void* eventData, const void* method) {
    const bool spot = IsWorldMapSpotItem(handler);
    int clickCount = 0;
    bool dbl = false;
    if (spot) dbl = NoteClickAndIsDouble(handler, eventData, &clickCount);
    FnExecutePointerDown orig = gOrigExecuteDown;
    if (orig) {
        __try {
            orig(handler, eventData, method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (!spot) return;
    if (dbl) {
        x::runtime::LogI("WorldMapTravel", "Spot 双击判定(ee) self=%p clickCount=%d", handler,
                         clickCount);
        FireGotoFromItem(handler, eventData);
        return;
    }
    static DWORD s_lastProbe = 0;
    static uint32_t s_clicks = 0;
    ++s_clicks;
    const DWORD now = GetTickCount();
    if (s_clicks <= 8 || now - s_lastProbe >= kClickProbeLogMs) {
        s_lastProbe = now;
        x::runtime::LogI("WorldMapTravel", "Spot 单击(ee) self=%p clickCount=%d n=%u winMs=%u",
                         handler, clickCount, s_clicks, DblClickWindowMs());
    }
}

bool TryInstallExecuteEventsDown() {
    void* eeKlass = FindClass("UnityEngine.EventSystems", "ExecuteEvents");
    void* ipdKlass = FindClass("UnityEngine.EventSystems", "IPointerDownHandler");
    if (!eeKlass || !ipdKlass) return false;
    SafeRuntimeClassInit(eeKlass);

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    using x::runtime::il2cpp_method::ResolvePath;
    MethodShape shape{};
    shape.arity = 2;
    shape.ret = TypeKind::Void;
    shape.unique = true;
    shape.walkParents = false;
    shape.param[0] = TypeKind::Ptr;
    shape.param[1] = TypeKind::Ptr;
    shape.paramKlass[0] = ipdKlass;

    ResolvePath path{};
    gMiExecuteDown =
        ResolveMi(eeKlass, kRvaExecutePointerDown, shape, "Execute", nullptr, &path);
    if (!gMiExecuteDown || !gMiExecuteDown->methodPointer) {
        gMiExecuteDown = nullptr;
        return false;
    }

    void* hook = reinterpret_cast<void*>(&Hook_ExecutePointerDown);
    void* miOrig = nullptr;
    if (!PatchMethodInfo(gMiExecuteDown, hook, &miOrig)) {
        gMiExecuteDown = nullptr;
        return false;
    }
    gOrigExecuteDown = reinterpret_cast<FnExecutePointerDown>(miOrig);

    // s_PointerDownHandler 委托在 cctor 时缓存了 method_ptr；只改 MI 不够。
    if (ResolveDelegateOffsets()) {
        const auto& e = x::runtime::il2cpp::Get();
        size_t offDel = kFbSPointerDownHandler;
        (void)FieldOffOrFb(eeKlass, "s_PointerDownHandler", kFbSPointerDownHandler, &offDel);
        void* statics = nullptr;
        if (e.classStaticData) {
            __try {
                statics = e.classStaticData(eeKlass);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                statics = nullptr;
            }
        }
        if (statics && offDel) {
            void* del = nullptr;
            __try {
                del = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(statics) + offDel);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                del = nullptr;
            }
            if (LooksLikeHeapPtr(del)) {
                void* curMp = nullptr;
                void* curInv = nullptr;
                __try {
                    curMp = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(del) + gOffMethodPtr);
                    if (gOffInvokeImpl)
                        curInv =
                            *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(del) + gOffInvokeImpl);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    curMp = nullptr;
                    curInv = nullptr;
                }
                gPointerDownDelegate = del;
                gDelegateMpSaved = curMp;
                gDelegateInvSaved = curInv;
                WritePtrField(del, gOffMethodPtr, hook);
                // invoke_impl 若仍指向旧原生 Execute，一并改到 Hook（部分 IL2CPP 走 invoke_impl）
                if (gOffInvokeImpl && curInv &&
                    (curInv == miOrig || PtrRva(curInv) == kRvaExecutePointerDown))
                    WritePtrField(del, gOffInvokeImpl, hook);
            }
        }
    }

    x::runtime::LogI("WorldMapTravel", "ExecuteEvents.PointerDown MI+delegate path=%s del=%p",
                     path != ResolvePath::Miss ? "meta" : "fallback", gPointerDownDelegate);
    return true;
}

bool TryInstallItemVtableDown() {
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    using x::runtime::il2cpp_method::ResolvePath;
    constexpr MethodShape kDn{1, TypeKind::Void, true, true, {TypeKind::Ptr}};
    ResolvePath pDn{};
    if (!gMiDown)
        gMiDown = ResolveMi(gItemKlass, kRvaOnPointerDown, kDn, "OnPointerDown", nullptr, &pDn);

    void* downNative = nullptr;
    if (gMiDown) {
        __try {
            downNative = gMiDown->methodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            downNative = nullptr;
        }
    }
    if (!downNative) downNative = AtRva<void*>(kRvaOnPointerDown);
    if (!downNative) return false;

    const char* slotPath = "miss";
    void** slots[kDownSlotCap]{};
    size_t offs[kDownSlotCap]{};
    const int nSlots = FindDownVtableSlotsOnHierarchy(
        gItemKlass, gMiDown, downNative, kRvaOnPointerDown, slots, offs, kDownSlotCap, &slotPath);

    void* hook = reinterpret_cast<void*>(&Hook_OnPointerDown);
    void* firstOrig = nullptr;
    int patched = 0;
    for (int i = 0; i < nSlots; ++i) {
        void* orig = nullptr;
        if (!PatchVtableMethodPtr(slots[i], hook, &orig)) continue;
        if (!firstOrig) firstOrig = orig;
        else if (orig != firstOrig) {
            RestoreVtableMethodPtr(slots[i], orig);
            continue;
        }
        gDownSlots[patched] = slots[i];
        gDownSlotOffs[patched] = offs[i];
        ++patched;
    }
    gDownSlotCount = patched;
    if (gDownSlotCount <= 0) return false;

    gDownNativeOrig = firstOrig ? firstOrig : downNative;
    gOrigDown = reinterpret_cast<FnOnPointerDown>(gDownNativeOrig);
    if (gMiDown) {
        void* miOrig = nullptr;
        (void)PatchMethodInfo(gMiDown, hook, &miOrig);
    }
    x::runtime::LogI("WorldMapTravel", "OnPointerDown vtable path=%s n=%d mi=%s", slotPath,
                     gDownSlotCount, pDn != ResolvePath::Miss ? "meta" : "fallback");
    return true;
}

void RememberAuxSlot(void** slot, void* orig) {
    if (!slot || !orig || gAuxSlotCount >= kAuxSlotCap) return;
    gAuxSlots[gAuxSlotCount] = slot;
    gAuxOrigs[gAuxSlotCount] = orig;
    ++gAuxSlotCount;
}

void PatchDeclaredVtable(void* klass, MethodInfoHead* mi, void* nativeOrig, void* hook) {
    if (!klass || !hook) return;
    const char* path = "miss";
    void** slots[kDownSlotCap]{};
    size_t offs[kDownSlotCap]{};
    const uint32_t rva = PtrRva(nativeOrig);
    const int n =
        FindDownVtableSlots(klass, mi, nativeOrig, rva, slots, offs, kDownSlotCap, &path);
    (void)path;
    for (int i = 0; i < n; ++i) {
        void* orig = nullptr;
        if (!PatchVtableMethodPtr(slots[i], hook, &orig)) continue;
        RememberAuxSlot(slots[i], orig);
    }
}

void ClearAuxHooks() {
    void* labelClickHook = reinterpret_cast<void*>(&Hook_LabelClick);
    void* enableHook = reinterpret_cast<void*>(&Hook_WmOnEnable);
    void* disableHook = reinterpret_cast<void*>(&Hook_WmOnDisable);
    void* bindHook = reinterpret_cast<void*>(&Hook_LabelBind);
    for (int i = 0; i < gAuxSlotCount; ++i) {
        if (!gAuxSlots[i] || !gAuxOrigs[i]) continue;
        void* cur = nullptr;
        __try {
            cur = *gAuxSlots[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cur = nullptr;
        }
        if (cur == labelClickHook || cur == enableHook || cur == disableHook || cur == bindHook)
            RestoreVtableMethodPtr(gAuxSlots[i], gAuxOrigs[i]);
        gAuxSlots[i] = nullptr;
        gAuxOrigs[i] = nullptr;
    }
    gAuxSlotCount = 0;

    if (gMiLabelBind && gOrigLabelBind)
        RestoreMethodInfo(gMiLabelBind, reinterpret_cast<void*>(gOrigLabelBind));
    if (gMiLabelClick && gOrigLabelClick)
        RestoreMethodInfo(gMiLabelClick, reinterpret_cast<void*>(gOrigLabelClick));
    if (gMiWmEnable && gOrigWmEnable)
        RestoreMethodInfo(gMiWmEnable, reinterpret_cast<void*>(gOrigWmEnable));
    if (gMiWmDisable && gOrigWmDisable)
        RestoreMethodInfo(gMiWmDisable, reinterpret_cast<void*>(gOrigWmDisable));
    gMiLabelBind = nullptr;
    gOrigLabelBind = nullptr;
    gMiLabelClick = nullptr;
    gOrigLabelClick = nullptr;
    gMiWmEnable = nullptr;
    gOrigWmEnable = nullptr;
    gMiWmDisable = nullptr;
    gOrigWmDisable = nullptr;
    gMiWmClose = nullptr;
    gWorldMapUi = nullptr;
}

bool TryInstallAuxHooks() {
    int n = 0;
    if (!gUiWmKlass) gUiWmKlass = FindClass("", kUiWorldMapClass);
    if (gUiWmKlass) {
        SafeRuntimeClassInit(gUiWmKlass);
        EnsureWmFieldOffsets(gUiWmKlass);
        gMiWmEnable = FindDeclaredMethod(gUiWmKlass, "OnEnable");
        gMiWmDisable = FindDeclaredMethod(gUiWmKlass, "OnDisable");
        void* origEn = nullptr;
        if (gMiWmEnable &&
            PatchMethodInfo(gMiWmEnable, reinterpret_cast<void*>(&Hook_WmOnEnable), &origEn)) {
            gOrigWmEnable = reinterpret_cast<FnWmEnable>(origEn);
            PatchDeclaredVtable(gUiWmKlass, gMiWmEnable, origEn,
                                reinterpret_cast<void*>(&Hook_WmOnEnable));
            ++n;
        } else {
            x::runtime::LogW("WorldMapTravel", "UIWorldMap.OnEnable 未装上（空 Spot 标题兜底可能失效）");
        }
        if (EnsureWmCloseMi()) {
            x::runtime::LogI("WorldMapTravel", "UIWorldMap.Close 已解析 mi=%p mp=%p（确认赶路时调用，不换桩）",
                             (void*)gMiWmClose, gMiWmClose->methodPointer);
        } else {
            x::runtime::LogW("WorldMapTravel",
                             "UIWorldMap.Close 未找到（确认赶路时跳过关地图，仍会 RequestGoto）");
        }
        void* origDis = nullptr;
        if (gMiWmDisable &&
            PatchMethodInfo(gMiWmDisable, reinterpret_cast<void*>(&Hook_WmOnDisable), &origDis)) {
            gOrigWmDisable = reinterpret_cast<FnWmEnable>(origDis);
            PatchDeclaredVtable(gUiWmKlass, gMiWmDisable, origDis,
                                reinterpret_cast<void*>(&Hook_WmOnDisable));
            ++n;
        }
    } else {
        x::runtime::LogW("WorldMapTravel", "UIWorldMap klass 未找到，跳过标题兜底钩");
    }

    if (!gLabelKlass) gLabelKlass = FindClass("", kLabelClass);
    if (gLabelKlass) {
        SafeRuntimeClassInit(gLabelKlass);
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kBind{2, TypeKind::Void, true, false, {TypeKind::Ptr, TypeKind::Ptr}};
        if (!gMiLabelBind)
            gMiLabelBind =
                ResolveMi(gLabelKlass, kRvaLabelBind, kBind, nullptr, kHashLabelBind);
        if (!gMiLabelBind) gMiLabelBind = FindMethodByRva(gLabelKlass, kRvaLabelBind);
        void* origBind = nullptr;
        if (gMiLabelBind &&
            PatchMethodInfo(gMiLabelBind, reinterpret_cast<void*>(&Hook_LabelBind), &origBind)) {
            gOrigLabelBind = reinterpret_cast<FnLabelBind>(origBind);
            ++n;
        }

        if (!gMiLabelClick) gMiLabelClick = FindDeclaredMethod(gLabelKlass, "OnPointerClick");
        if (!gMiLabelClick) gMiLabelClick = FindMethodByRva(gLabelKlass, kRvaLabelClick);
        void* origClick = nullptr;
        if (gMiLabelClick &&
            PatchMethodInfo(gMiLabelClick, reinterpret_cast<void*>(&Hook_LabelClick), &origClick)) {
            gOrigLabelClick = reinterpret_cast<FnLabelClick>(origClick);
            PatchDeclaredVtable(gLabelKlass, gMiLabelClick, origClick,
                                reinterpret_cast<void*>(&Hook_LabelClick));
            ++n;
        } else {
            x::runtime::LogW("WorldMapTravel", "标题点 OnPointerClick 未装上（点文字可能仍无赶路）");
        }
    } else {
        x::runtime::LogW("WorldMapTravel", "标题点 klass 未找到，跳过 Label 钩");
    }

    x::runtime::LogI("WorldMapTravel", "aux hooks n=%d uiWm=%p label=%p enable=%d click=%d", n,
                     gUiWmKlass, gLabelKlass, gOrigWmEnable ? 1 : 0, gOrigLabelClick ? 1 : 0);
    return n > 0;
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

    // 只装一条热路径，避免 Execute→vtable 双触发双计数。
    const bool eeOk = TryInstallExecuteEventsDown();
    const bool vtOk = eeOk ? false : TryInstallItemVtableDown();
    if (!eeOk && !vtOk) {
        RestoreMethodInfo(gMiUpdate, origUv);
        gOrigUpdate = nullptr;
        ClearDownHooks();
        x::runtime::LogW("WorldMapTravel",
                         "OnPointerDown 数据面钩失败（ExecuteEvents+vtable）；拒绝 abs/.text");
        x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "Down data miss");
        return false;
    }

    snprintf(gDownPath, sizeof(gDownPath), "ee=%d vt=%d", eeOk ? 1 : 0, vtOk ? 1 : 0);
    gInstalled.store(true);
    (void)TryInstallAuxHooks();
    x::runtime::LogI("WorldMapTravel",
                     "init[经典版]：UpdateView(MI)+OnPointerDown(%s) 无.text；"
                     "双击 Spot → YesNo → 冻结遇人 → Close世界地图 → RequestGoto；空 Spot 仅失败路径兜底",
                     gDownPath);
    x::runtime::anchor_lamps::Set("WorldMap", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                 gDownPath);
    return true;
}

void Uninstall() {
    if (!gInstalled.exchange(false)) return;
    ClearAuxHooks();
    ClearDownHooks();
    if (gMiUpdate && gOrigUpdate) RestoreMethodInfo(gMiUpdate, reinterpret_cast<void*>(gOrigUpdate));
    gOrigUpdate = nullptr;
    gMiUpdate = nullptr;
    {
        std::lock_guard<std::mutex> lock(gMu);
        gSpotByItem.clear();
        gItemByGo.clear();
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
        } else if (gDownSlotCount > 0 && gDownNativeOrig) {
            // 类再 init / 别处重写虚表时重新钉回 Hook
            void* hook = reinterpret_cast<void*>(&Hook_OnPointerDown);
            for (int i = 0; i < gDownSlotCount; ++i) {
                if (!gDownSlots[i]) continue;
                void* cur = nullptr;
                __try {
                    cur = *gDownSlots[i];
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    cur = nullptr;
                }
                if (cur && cur != hook) {
                    void* ignored = nullptr;
                    if (PatchVtableMethodPtr(gDownSlots[i], hook, &ignored)) {
                        x::runtime::LogI("WorldMapTravel", "OnPointerDown re-pin slot off=0x%zX",
                                         gDownSlotOffs[i]);
                    }
                }
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
