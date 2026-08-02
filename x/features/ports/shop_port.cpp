// Classic TWMS shop_port — UIShopDialog ready + TalkToNpc + UI 买卖。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shop_port.h"

#include "travel_port.h"
#include "world_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_prefab.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/anchor_lamps.h"
#include "../travel/travel.h"
#include "xcat_item_catalog.h"

#include <Windows.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace x::features::ports::shop {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// UIShopDialog — Prefab TypeDef
constexpr char kUiShopDialogClass[] =
    "b22d9c09d3f22e06cc2223c664889c14b3536d0dc1cae5d4720b1d5ba784d8d";
constexpr char kPrefabShopDialog[] = "UIShopDialog";
// NpcPool — remounted 2026-08-03（旧 b7508d58… 已不在 dump；形：List@10 Dict@18 List@20 List@28 Field@30 int@38）
constexpr char kNpcPoolClass[] =
    "a7a52ebb1d1a06fb72807c849d52cd19734ad00e28b8d36a7d31c1fb3bd9f61";
// UIUtilDialogEx（脚本对话 / AskMenu）— Prefab TypeDefIndex 609
constexpr char kUiUtilDialogExClass[] =
    "f6aae16821215515c8a8a544c9190ebfe831448d8d38f05740f1499231da1ae";
constexpr char kPrefabUtilDialogEx[] = "UIUtilDialogEx";
constexpr char kFuncKeyClass[] =
    "bd3a79401c7d64bce45aac35ca0daf6e2dc938d1ffc0b396e137fde02b8c4cf";

constexpr uint32_t kRvaOutPacketCreate = 0x1CB7BB0;  // remapped 2026-08-03
constexpr uint32_t kRvaOutPacketEncode1Byte = 0x1CC4040;  // remapped 2026-08-03
constexpr uint32_t kRvaOutPacketEncode2Short = 0x1CC4370;  // remapped 2026-08-03
constexpr uint32_t kRvaOutPacketEncode4Int = 0x1CC4480;  // remapped 2026-08-03
constexpr uint32_t kRvaNmSend = 0x1CB98B0;  // remapped 2026-08-03
constexpr uint32_t kRvaUserLocalTalkToNpc = 0x107E9A0;  // remapped 2026-08-03
constexpr uint32_t kRvaOnFuncKey = 0x107A200;  // remapped 2026-08-03
constexpr uint32_t kRvaFuncKeyCtor = 0x1641AE0;  // remapped 2026-08-03: .ctor(FuncType,int)
// UIUtilDialogEx：SetKeyFocus(int) 选菜单项 / OnClickBtOk（走脚本应答链，含 66）
constexpr uint32_t kRvaUiDlgSelectMenu = 0x786020;  // remapped 2026-08-03 SetKeyFocus
constexpr uint32_t kRvaUiDlgOnClickBtOk = 0x78E780;  // remapped 2026-08-03 OnClickBtOk
// UIShopDialog.SendSellRequestPacket(int) — 产品卖出入口（与手组包同源，含 UI 状态）
constexpr uint32_t kRvaSendSellRequestPacket = 0x54B700;  // remapped 2026-08-03
// UIShopDialog.SendBuyRequestPacket(int) — 产品买入入口（见 docs/.../P0c）
constexpr uint32_t kRvaSendBuyRequestPacket = 0x54AAE0;  // remapped 2026-08-03
// UIShopDialog.CmpSellItem — 对照背包刷新卖栏列表（开店后列表=可卖背包投影）
constexpr uint32_t kRvaCmpSellItem = 0x54D6A0;  // remapped 2026-08-03
// UIShopDialog.SetRet — 离店/收口（CMS 同源；私有，走官方关店链）
constexpr uint32_t kRvaShopSetRet = 0x5376A0;  // remapped 2026-08-03
// UIDialog.Close — 基类虚函数 Slot:31；关 UI 实例
constexpr uint32_t kRvaUiDialogClose = 0x116F830;  // remapped 2026-08-03 UIDialog.Close Slot:31
// 方法哈希（dump 可读名缺失时的防漂；void(int) 在 UIShopDialog 上不唯一）
constexpr char kHashSendSell[] =
    "b0a44be8c4bbd2a385facfe0ec6cb20e751012177bced08bafb90254d22b384";
constexpr char kHashSendBuy[] =
    "aff86c0c4a220985149cc3d5d02afd88b5da89682f1386e2a554b7cd35a1081";
constexpr char kHashCmpSell[] =
    "aa345f6c1d838a471d287f0b0837bac692ba7c89eb3b27f408f3aff8a2bc652";
constexpr char kHashSendPacket[] =
    "bcd0d90687418e2b3ff0faf5b96a9bb4028720a4eb37b78b74b873ce1dd891f";
constexpr char kHashTalkToNpc[] =
    "a3cfea1a1aedb69214dbab1263c0d84c212e9c14bbc8620b9b5f64cc5867420";
constexpr char kHashOnFuncKey[] =
    "eb70dd6a52329f9f7cffa938d48f1c529af67d1705bba4507ade9d5f58eabbe";
constexpr char kHashUiTabOnClick[] =
    "ef6e8a74325e1b8968f7d98baa3efc8c8886d658893d3cb9a04dd4c73714975";
constexpr char kOutPacketClass[] =
    "aeb7167893ac51cbc0cf730326f2361e6e8b797eeb940786711185ef0fd658c";
// Unity helpers（明文名稳定；无游戏侧哈希名）— 走 ResolveUnityMi，禁止裸 AtRva 主路径
// Button.Press — 触发 onClick（Awake 里 buttonExit→SetRet）
constexpr uint32_t kRvaButtonPress = 0x4FA8800;  // remapped 2026-08-03
// Component.get_gameObject / GameObject.SetActive / get_activeSelf — 残留 modal 强拆
constexpr uint32_t kRvaGetGameObject = 0x4E47E00;  // remapped 2026-08-03
constexpr uint32_t kRvaGoSetActive = 0x4E4D6A0;  // remapped 2026-08-03 · 勿与 set_active@4E4D5D0 混淆
constexpr uint32_t kRvaGoGetActiveSelf = 0x4E4D770;  // remapped 2026-08-03
// UITab.OnClickTab(int) — 切换商店角色区「装备/消耗/其他」等 TAB，并触发 OnTabChanged
constexpr uint32_t kRvaUiTabOnClickTab = 0xAC0800;  // remapped 2026-08-03
constexpr int kClientUserShopRequest = 67;
constexpr uint8_t kShopOpSell = 1;
constexpr uint8_t kShopOpBuy = 0;
constexpr int32_t kKeyInputDown = 0;
constexpr int32_t kKeyInputUp = 1;
constexpr int32_t kFuncTypeBasicAction = 5;
constexpr int32_t kFkmBasicActionNpcTalk = 54;
constexpr int kUiDlgTypeText = 0;
constexpr int kUiDlgTypeYesNo = 1;
constexpr int kUiDlgTypeList = 4;

// Session/NM 方法宿主（与 il2cpp_shape::kHashNetworkManager 同；EnsureBound 走 ResolveNetworkManagerKlass）
constexpr char kSessionClass[] =
    "f0ee06b64ad95c59b95ca923b6db62ce451a5c512b3ef47e7c3814caca41909";

constexpr size_t kOffWmCharacterData = 0xE0;
constexpr size_t kOffWmBasicStat = 0xE8;
constexpr size_t kOffCdItemSlots = 0x40;
constexpr size_t kOffCdCharacterStat = 0x10;
constexpr size_t kOffCsMoney = 0x58;  // CharacterStat.Money : long（2026-08-03）
constexpr size_t kOffListItems = 0x10;
constexpr size_t kOffListSize = 0x18;
constexpr size_t kOffSlotItemId = 0x10;
constexpr size_t kOffBundleNumber = 0x28;
constexpr size_t kOffNpcPoolList = 0x10;  // NpcPool._npcList
constexpr size_t kOffActorPos = 0x64;      // FieldActorBase Pos (Vector2)
constexpr size_t kOffNpcObjectId = 0x78;
constexpr size_t kOffNpcData = 0x80;     // Npc._npcData
constexpr size_t kOffNpcDataId = 0x10;   // NpcData.Id (= template)
constexpr size_t kOffUiDlgType = 0xA0;    // UIUtilDialogEx.Type
constexpr size_t kOffUiDlgMenuTexts = 0xE0;  // List<string> 菜单文案
constexpr size_t kOffCachedPtr = 0x10;
constexpr size_t kOffNmSession = 0x10;       // Facade → Session*
constexpr size_t kOffNmSessionState = 0x18;  // Facade.SessionState（3=Connected）
constexpr size_t kOffNmOpcodeHashSet = 0x48;
constexpr size_t kOffSessionState = 0x60;    // Session.SessionState（TW；真连线态）
constexpr size_t kOffOutPacketId = 0x20;
constexpr size_t kOffPacketOffset = 0x18;
// UIShopDialog（TW）：买/卖列表 / 选中下标 / 请求中标记
constexpr size_t kOffBuyItemList0 = 0x178;  // 买栏候选 A（UITab 二选一）
constexpr size_t kOffBuyItemList1 = 0x180;  // 买栏候选 B
constexpr size_t kOffSellItemList = 0x198;
constexpr size_t kOffBuySelectedIndex = 0x1A8;
constexpr size_t kOffLastBuyIndex = 0x1B0;
constexpr size_t kOffSellSelectedIndex = 0x1AC;
constexpr size_t kOffLastSellIndex = 0x1B8;
constexpr size_t kOffHasShopRequestSent = 0x1B4;
constexpr size_t kOffShopItemId = 0x10;     // UIShopDialog.Item.ItemId
constexpr size_t kOffShopItemPos = 0x14;    // UIShopDialog.Item.Position
constexpr size_t kOffShopItemPrice = 0x28;  // UIShopDialog.Item.Price
// UIShopDialog 两侧 UITab：0xC0 多为店侧模式；0xC8 多为角色背包区（装备/消耗/其他）
constexpr size_t kOffShopUiTab0 = 0xC0;
constexpr size_t kOffShopUiTab1 = 0xC8;
constexpr size_t kOffShopButtonExit = 0xA8;  // UIShopDialog.buttonExit : UIButton
constexpr size_t kOffUiTabCurrentIndex = 0x20;  // UITab.CurrentTabIndex
constexpr size_t kOffUiTabItems = 0x28;         // UITab.Items
constexpr int kSessionStateConnected = 3;

constexpr int kInvTiEquip = 1;
constexpr int kInvTiConsume = 2;
constexpr int kInvTiEtc = 4;
constexpr DWORD kJobWaitMs = 2000;
constexpr float kDefaultTalkDist = 220.f;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

using FnFindAll = void* (*)(void* type, void* method);
using FnOutCreate = void* (*)(int packetId, const void* method);
using FnEncode1 = void (*)(void* self, uint8_t v, const void* method);
using FnEncode2 = void (*)(void* self, int16_t v, const void* method);
using FnEncode4 = void (*)(void* self, int32_t v, const void* method);
using FnNmSend = bool (*)(void* self, void* packet, const void* method);
using FnTalkToNpc = void (*)(void* self, void* npc, const void* method);
using FnOnFuncKey = void (*)(void* self, int32_t inputType, void* funcKey, uint32_t scan,
                             const void* methodInfo);
using FnFuncKeyCtor = void (*)(void* self, int32_t funcType, int32_t value,
                               const void* methodInfo);
using FnUiDlgSelectMenu = void (*)(void* self, int index, const void* method);
using FnUiDlgOnClickOk = void (*)(void* self, const void* method);
using FnSendSellPacket = void (*)(void* self, int nCount, const void* method);
using FnSendBuyPacket = void (*)(void* self, int nCount, const void* method);
using FnCmpSellItem = int (*)(void* self, const void* method);
using FnShopSetRet = void (*)(void* self, const void* method);
using FnUiDialogClose = void (*)(void* self, const void* method);
using FnButtonPress = void (*)(void* self, const void* method);
using FnGetGameObject = void* (*)(void* self, const void* method);
using FnGoSetActive = void (*)(void* self, bool value, const void* method);
using FnGoGetActiveSelf = bool (*)(void* self, const void* method);
using FnUiTabOnClickTab = void (*)(void* self, int index, const void* method);

void* gGA = nullptr;
FnFindAll gFindAll = nullptr;
void* gShopDlgKlass = nullptr;
void* gShopDlgType = nullptr;
void* gFacadeKlass = nullptr;
void* gFacadeType = nullptr;
void* gSessionKlass = nullptr;
void* gNmKlass = nullptr;  // alias: Session klass（Send MethodInfo）
void* gNmType = nullptr;   // unused after facade；保留防漏改
void* gNm = nullptr;       // Session*（发包 this）
void* gNmFacade = nullptr; // NetworkManager facade 实例
void* gOutPacketKlass = nullptr;
void* gNpcPoolKlass = nullptr;
void* gNpcPool = nullptr;
void* gUiDlgKlass = nullptr;
void* gUiDlgType = nullptr;
MethodInfoHead* gMiOutCreate = nullptr;
DWORD gLastMenuLogMs = 0;
DWORD gLastFuncKeyLogMs = 0;
MethodInfoHead* gMiEncode1 = nullptr;
MethodInfoHead* gMiEncode2 = nullptr;
MethodInfoHead* gMiEncode4 = nullptr;
MethodInfoHead* gMiSend = nullptr;
MethodInfoHead* gMiSendSellPacket = nullptr;
MethodInfoHead* gMiSendBuyPacket = nullptr;
MethodInfoHead* gMiCmpSellItem = nullptr;
MethodInfoHead* gMiShopSetRet = nullptr;
MethodInfoHead* gMiUiDialogClose = nullptr;
MethodInfoHead* gMiButtonPress = nullptr;
MethodInfoHead* gMiGetGameObject = nullptr;
MethodInfoHead* gMiGoSetActive = nullptr;
MethodInfoHead* gMiGoGetActiveSelf = nullptr;
bool gUnityHelpersBound = false;
DWORD gLastUnityBindLogMs = 0;
void* gShopDlg = nullptr;
DWORD gLastRebindMs = 0;
DWORD gLastTalkLogMs = 0;

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

void WriteI32(void* obj, size_t off, int32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteU8(void* obj, size_t off, uint8_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

int16_t ReadI16(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int ListSize(void* list) {
    if (!LooksLikeHeapPtr(list)) return 0;
    return ReadI32(list, kOffListSize);
}

void* ListAt(void* list, int index) {
    if (!LooksLikeHeapPtr(list) || index < 0) return nullptr;
    void* items = ReadPtr(list, kOffListItems);
    if (!LooksLikeHeapPtr(items)) return nullptr;
    const uintptr_t n = ArrayLen(items);
    if (static_cast<uintptr_t>(index) >= n) return nullptr;
    return ArrayAt(items, static_cast<uintptr_t>(index));
}

int ItemQty(void* slot) {
    if (!LooksLikeHeapPtr(slot)) return 0;
    const int16_t n = ReadI16(slot, kOffBundleNumber);
    return n > 0 ? static_cast<int>(n) : 1;
}

void* FindClass(const char* name) {
    return x::runtime::il2cpp::FindClass("", name);
}

void* ClassTypeObject(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetType || !e.typeGetObject) return nullptr;
    __try {
        void* ty = e.classGetType(klass);
        if (!ty) return nullptr;
        return e.typeGetObject(ty);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool LooksLikeFacade(void* cand) {
    if (!LooksLikeHeapPtr(cand)) return false;
    if (gFacadeKlass) {
        void* k = ReadPtr(cand, 0);
        if (k != gFacadeKlass) return false;
    }
    void* sess = ReadPtr(cand, kOffNmSession);
    const int st = ReadI32(cand, kOffNmSessionState);
    if (st < 0 || st > 3) return false;
    if (sess && !LooksLikeHeapPtr(sess)) return false;
    if (LooksLikeHeapPtr(sess)) return true;
    return st == 2 || st == 3;  // Connecting / Connected
}

bool LooksLikeNm(void* cand) {
    // 历史名：实际校验 Session*（发包 this）
    if (!LooksLikeHeapPtr(cand)) return false;
    if (gSessionKlass) {
        void* k = ReadPtr(cand, 0);
        if (k != gSessionKlass) return false;
    }
    const int st = ReadI32(cand, kOffSessionState);
    return st >= 0 && st <= 3;
}

void* TryLazyValue(void* lazy) {
    if (!LooksLikeHeapPtr(lazy)) return nullptr;
    const size_t tryOffs[] = {0x10, 0x18, 0x20, 0x28, 0x08};
    for (size_t off : tryOffs) {
        void* v = ReadPtr(lazy, off);
        if (LooksLikeHeapPtr(v)) return v;
    }
    return nullptr;
}

void* ResolveSingleton(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    auto classInit = [&](void* k) {
        if (!k || !e.runtimeClassInit) return;
        __try {
            e.runtimeClassInit(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    };
    auto staticsOf = [&](void* k) -> void* {
        if (!k || !e.classStaticData) return nullptr;
        __try {
            void* sd = e.classStaticData(k);
            return LooksLikeHeapPtr(sd) ? sd : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    };
    auto pickFromStatics = [&](void* sd) -> void* {
        if (!sd) return nullptr;
        void* best = nullptr;
        for (size_t s = 0; s < 4; ++s) {
            void* lazy = ReadPtr(sd, s * sizeof(void*));
            void* cand = TryLazyValue(lazy);
            if (!cand) cand = lazy;
            if (!LooksLikeFacade(cand)) continue;
            void* sess = ReadPtr(cand, kOffNmSession);
            const int st = ReadI32(cand, kOffNmSessionState);
            if (LooksLikeHeapPtr(sess) && st == kSessionStateConnected) return cand;
            if (!best) best = cand;
        }
        return best;
    };

    classInit(klass);
    void* parent = nullptr;
    if (e.classParent) {
        __try {
            parent = e.classParent(klass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
    }
    if (parent) classInit(parent);

    // Prefer Singleton<> parent statics（kick_sniff 同路径）
    if (void* inst = pickFromStatics(staticsOf(parent))) return inst;
    if (void* inst = pickFromStatics(staticsOf(klass))) return inst;
    return nullptr;
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !gGA) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetMethods) return nullptr;
    const uintptr_t want = reinterpret_cast<uintptr_t>(gGA) + rva;
    void* iter = nullptr;
    __try {
        for (;;) {
            void* raw = e.classGetMethods(klass, &iter);
            if (!raw) break;
            auto* mi = reinterpret_cast<MethodInfoHead*>(raw);
            // Travel 会把 methodPointer 换成 Hook，但仍保留 virtualMethodPointer=原生。
            if (reinterpret_cast<uintptr_t>(mi->methodPointer) == want ||
                reinterpret_cast<uintptr_t>(mi->virtualMethodPointer) == want) {
                return mi;
            }
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

// 明文名 → 方法哈希 → RVA+kind（paramKlass 可钉死 OutPacket 等）。
MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plainName = nullptr, const char* hashName = nullptr) {
    if (plainName) {
        if (MethodInfoHead* mi = FindMethodByName(klass, plainName, shape.arity)) return mi;
    }
    if (hashName) {
        if (MethodInfoHead* mi = FindMethodByName(klass, hashName, shape.arity)) return mi;
    }
    if (!klass) return FindMethodByRva(klass, rva);
    const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("Shop", "ResolveMi kind hit rva=0x%X plain=%s", rva,
                             plainName ? plainName : "-");
        }
        return reinterpret_cast<MethodInfoHead*>(mr.method);
    }
    return FindMethodByRva(klass, rva);
}

bool ResolveApi();

// Unity：RVA 优先（SetActive 与 set_active 同形）；明文名 remount 兜底。
MethodInfoHead* ResolveUnityMi(void* klass, uint32_t rva, const char* plain, int arity,
                               const x::runtime::il2cpp_method::MethodShape& shape) {
    if (klass) {
        const auto mr = x::runtime::il2cpp_method::FindMethodCached(klass, rva, shape);
        if (mr.method) {
            if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
                x::runtime::LogI("Shop", "ResolveUnityMi kind hit rva=0x%X plain=%s", rva,
                                 plain ? plain : "-");
            }
            return reinterpret_cast<MethodInfoHead*>(mr.method);
        }
    }
    if (MethodInfoHead* mi = FindMethodByName(klass, plain, arity)) return mi;
    return nullptr;
}

template <typename Fn>
Fn FnFromMi(MethodInfoHead* mi, uint32_t rva) {
    if (mi && mi->methodPointer) return reinterpret_cast<Fn>(mi->methodPointer);
    return AtRva<Fn>(rva);
}

bool BindUnityHelpers() {
    if (gUnityHelpersBound && gMiButtonPress && gMiGetGameObject && gMiGoSetActive &&
        gMiGoGetActiveSelf)
        return true;
    if (!ResolveApi()) return false;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    void* btnKlass = x::runtime::il2cpp::FindClass("UnityEngine.UI", "Button");
    void* compKlass = x::runtime::il2cpp::FindClass("UnityEngine", "Component");
    void* goKlass = x::runtime::il2cpp::FindClass("UnityEngine", "GameObject");
    if (btnKlass && !gMiButtonPress) {
        constexpr MethodShape kPress{0, TypeKind::Void, true, true, {}};
        gMiButtonPress = ResolveUnityMi(btnKlass, kRvaButtonPress, "Press", 0, kPress);
    }
    if (compKlass && !gMiGetGameObject) {
        constexpr MethodShape kGo{0, TypeKind::Ptr, true, true, {}};
        gMiGetGameObject =
            ResolveUnityMi(compKlass, kRvaGetGameObject, "get_gameObject", 0, kGo);
    }
    if (goKlass) {
        // SetActive 与 set_active 同 void(bool)，unique kind 不可靠 → RVA+明文
        if (!gMiGoSetActive) {
            constexpr MethodShape kSet{1, TypeKind::Void, false, true, {TypeKind::Bool}};
            gMiGoSetActive =
                ResolveUnityMi(goKlass, kRvaGoSetActive, "SetActive", 1, kSet);
        }
        if (!gMiGoGetActiveSelf) {
            constexpr MethodShape kAct{0, TypeKind::Bool, true, true, {}};
            gMiGoGetActiveSelf =
                ResolveUnityMi(goKlass, kRvaGoGetActiveSelf, "get_activeSelf", 0, kAct);
        }
    }
    gUnityHelpersBound =
        gMiButtonPress && gMiGetGameObject && gMiGoSetActive && gMiGoGetActiveSelf;
    const DWORD now = GetTickCount();
    if (now - gLastUnityBindLogMs > 15000) {
        gLastUnityBindLogMs = now;
        if (gUnityHelpersBound) {
            x::runtime::LogI(
                "Shop",
                "unity helper bind ok mi(press=%d go=%d set=%d act=%d) Press@0x%X "
                "get_gameObject@0x%X SetActive@0x%X get_activeSelf@0x%X",
                gMiButtonPress ? 1 : 0, gMiGetGameObject ? 1 : 0, gMiGoSetActive ? 1 : 0,
                gMiGoGetActiveSelf ? 1 : 0, kRvaButtonPress, kRvaGetGameObject, kRvaGoSetActive,
                kRvaGoGetActiveSelf);
            x::runtime::anchor_lamps::Set("ShopUnity", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                         "mi 4/4");
        } else {
            x::runtime::LogW(
                "Shop",
                "unity helper bind partial mi(press=%d go=%d set=%d act=%d) — FnFromMi RVA fallback",
                gMiButtonPress ? 1 : 0, gMiGetGameObject ? 1 : 0, gMiGoSetActive ? 1 : 0,
                gMiGoGetActiveSelf ? 1 : 0);
            const int n = (gMiButtonPress ? 1 : 0) + (gMiGetGameObject ? 1 : 0) +
                          (gMiGoSetActive ? 1 : 0) + (gMiGoGetActiveSelf ? 1 : 0);
            char detail[48]{};
            snprintf(detail, sizeof(detail), "mi %d/4", n);
            x::runtime::anchor_lamps::Set(
                "ShopUnity",
                n > 0 ? x::runtime::anchor_lamps::AnchorLampCode::Degraded
                      : x::runtime::anchor_lamps::AnchorLampCode::Miss,
                detail);
        }
    }
    return gUnityHelpersBound || gGA != nullptr;
}

// 直调 GameAssembly 原生 Send，绕过 Travel 的 MethodInfo Hook。
FnNmSend ResolveSendFn() {
    if (gMiSend && gMiSend->methodPointer) {
        // Travel 可能已把 methodPointer 换成 hook；优先 virtualMethodPointer=原生。
        if (gMiSend->virtualMethodPointer) {
            return reinterpret_cast<FnNmSend>(gMiSend->virtualMethodPointer);
        }
    }
    return AtRva<FnNmSend>(kRvaNmSend);
}

bool ResolveApi() {
    if (gGA && gFindAll) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    return gFindAll != nullptr;
}

bool Rebind(DWORD now) {
    if (now - gLastRebindMs < 1500 && gShopDlgKlass &&
        (gMiSendSellPacket || (gGA && AtRva<void*>(kRvaSendSellRequestPacket))) &&
        (gMiSendBuyPacket || (gGA && AtRva<void*>(kRvaSendBuyRequestPacket))))
        return true;
    gLastRebindMs = now;
    if (!ResolveApi()) return false;
    BindUnityHelpers();

    if (!gShopDlgKlass)
        gShopDlgKlass =
            x::runtime::il2cpp_prefab::FindClassCached(kUiShopDialogClass, kPrefabShopDialog).klass;
    if (!gShopDlgType && gShopDlgKlass) gShopDlgType = ClassTypeObject(gShopDlgKlass);

    if (!gFacadeKlass) gFacadeKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
    if (!gSessionKlass) gSessionKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
    gNmKlass = gSessionKlass;  // Send MethodInfo 宿主
    if (!gFacadeType && gFacadeKlass) gFacadeType = ClassTypeObject(gFacadeKlass);
    if (!gOutPacketKlass) {
        gOutPacketKlass = FindClass("OutPacket");
        if (!gOutPacketKlass) gOutPacketKlass = FindClass(kOutPacketClass);
        if (!gOutPacketKlass)
            gOutPacketKlass =
                FindClass("f217b52888ce36bdb81e5951edbb513b60965a09e129a599bf1fab00e86a590");
    }

    if (gNm && !LooksLikeNm(gNm)) gNm = nullptr;
    if (gNmFacade && !LooksLikeFacade(gNmFacade)) gNmFacade = nullptr;
    if (!gNmFacade) gNmFacade = ResolveSingleton(gFacadeKlass);
    // 已在主线程 job 内：FindAll 直调，禁止再套 managed_main::FindAll（嵌套 InvokeAndWait 会死锁）。
    if (!gNmFacade && gFacadeType && gFindAll) {
        void* arr = nullptr;
        __try {
            arr = gFindAll(gFacadeType, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            arr = nullptr;
        }
        const int n = LooksLikeHeapPtr(arr)
                          ? static_cast<int>(*reinterpret_cast<uintptr_t*>(
                                reinterpret_cast<uint8_t*>(arr) + 0x18))
                          : 0;
        for (int i = 0; i < n && i < 8; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                                static_cast<size_t>(i) * sizeof(void*));
            if (!LooksLikeFacade(o)) continue;
            gNmFacade = o;
            break;
        }
    }
    if (!gNm && gNmFacade) {
        void* sess = ReadPtr(gNmFacade, kOffNmSession);
        if (LooksLikeHeapPtr(sess)) gNm = sess;
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;

    if (gOutPacketKlass) {
        // static OutPacket Create(PacketType/int) — dump 上唯一 static ptr(ptr|i32) 近似用 Ptr。
        constexpr MethodShape kCreate{1, TypeKind::Ptr, true, false, {TypeKind::Any}};
        if (!gMiOutCreate)
            gMiOutCreate =
                ResolveMi(gOutPacketKlass, kRvaOutPacketCreate, kCreate, "Create", nullptr);
        // Encode* 同形多（byte/short/int）→ RVA 主路径 + kind 软验（Any）。
        constexpr MethodShape kEnc{1, TypeKind::Void, true, false, {TypeKind::Any}};
        if (!gMiEncode1)
            gMiEncode1 = ResolveMi(gOutPacketKlass, kRvaOutPacketEncode1Byte, kEnc, nullptr, nullptr);
        if (!gMiEncode2)
            gMiEncode2 =
                ResolveMi(gOutPacketKlass, kRvaOutPacketEncode2Short, kEnc, nullptr, nullptr);
        if (!gMiEncode4) {
            constexpr MethodShape kEnc4{1, TypeKind::Void, true, false, {TypeKind::I32}};
            gMiEncode4 =
                ResolveMi(gOutPacketKlass, kRvaOutPacketEncode4Int, kEnc4, nullptr, nullptr);
        }
    }
    if (gNmKlass && !gMiSend) {
        // bool(OutPacket) — paramKlass 钉死，Session 上多个 bool(ptr) 可唯一。
        MethodShape kSend{};
        kSend.arity = 1;
        kSend.ret = TypeKind::Bool;
        kSend.unique = true;
        kSend.walkParents = true;
        kSend.param[0] = TypeKind::Ptr;
        if (gOutPacketKlass) kSend.paramKlass[0] = gOutPacketKlass;
        gMiSend = ResolveMi(gNmKlass, kRvaNmSend, kSend, "SendPacket", kHashSendPacket);
        if (!gMiSend) gMiSend = ResolveMi(gNmKlass, kRvaNmSend, kSend, "Send", kHashSendPacket);
    }
    if (gShopDlgKlass && !gMiSendSellPacket) {
        constexpr MethodShape kSell{1, TypeKind::Void, true, false, {TypeKind::I32}};
        gMiSendSellPacket = ResolveMi(gShopDlgKlass, kRvaSendSellRequestPacket, kSell,
                                      "SendSellRequestPacket", kHashSendSell);
    }
    if (gShopDlgKlass && !gMiSendBuyPacket) {
        constexpr MethodShape kBuy{1, TypeKind::Void, true, false, {TypeKind::I32}};
        gMiSendBuyPacket = ResolveMi(gShopDlgKlass, kRvaSendBuyRequestPacket, kBuy,
                                     "SendBuyRequestPacket", kHashSendBuy);
    }
    if (gShopDlgKlass && !gMiCmpSellItem) {
        constexpr MethodShape kCmp{0, TypeKind::I32, true, false, {}};
        gMiCmpSellItem =
            ResolveMi(gShopDlgKlass, kRvaCmpSellItem, kCmp, "CmpSellItem", kHashCmpSell);
    }
    if (gShopDlgKlass && !gMiShopSetRet) {
        constexpr MethodShape kRet{0, TypeKind::Void, true, false, {}};
        gMiShopSetRet = ResolveMi(gShopDlgKlass, kRvaShopSetRet, kRet, "SetRet", nullptr);
    }
    // Close 在 UIDialog 基类：walkParents + 明文 Close。
    if (!gMiUiDialogClose && gShopDlgKlass) {
        MethodShape kClose{};
        kClose.arity = 0;
        kClose.ret = TypeKind::Void;
        kClose.unique = true;
        kClose.walkParents = true;
        gMiUiDialogClose = ResolveMi(gShopDlgKlass, kRvaUiDialogClose, kClose, "Close", nullptr);
    }
    // 买/卖均走 UIShopDialog.*RequestPacket；手组包已证实会本地踢线。
    return gShopDlgKlass && (gMiSendSellPacket || AtRva<void*>(kRvaSendSellRequestPacket)) &&
           (gMiSendBuyPacket || AtRva<void*>(kRvaSendBuyRequestPacket));
}

void* GetBagList(int invType) {
    void* wm = world::GetWorldManager();
    if (!wm) return nullptr;
    void* cd = ReadPtr(wm, kOffWmCharacterData);
    if (!LooksLikeHeapPtr(cd)) return nullptr;
    void* slotsArr = ReadPtr(cd, kOffCdItemSlots);
    if (!LooksLikeHeapPtr(slotsArr)) return nullptr;
    const uintptr_t n = ArrayLen(slotsArr);
    if (static_cast<uintptr_t>(invType) >= n) return nullptr;
    return ArrayAt(slotsArr, static_cast<uintptr_t>(invType));
}

struct ReadyJob {
    bool ready = false;
};

bool UnityAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    __try {
        return ReadPtr(obj, kOffCachedPtr) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// FindAll 关店后仍可能扫到实例；须 Unity 存活且 GameObject.activeSelf。
bool ShopDlgLooksOpen(void* dlg) {
    if (!UnityAlive(dlg)) return false;
    BindUnityHelpers();
    auto getGo = FnFromMi<FnGetGameObject>(gMiGetGameObject, kRvaGetGameObject);
    auto getActive = FnFromMi<FnGoGetActiveSelf>(gMiGoGetActiveSelf, kRvaGoGetActiveSelf);
    if (!getGo || !getActive) return true;  // 无法校验时保守当作仍开着
    void* go = nullptr;
    __try {
        go = getGo(dlg, gMiGetGameObject);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        go = nullptr;
    }
    if (!go) return true;
    bool active = true;
    __try {
        active = getActive(go, gMiGoGetActiveSelf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        active = true;
    }
    return active;
}

void ReadyJobOnMain(void* user) {
    auto* job = reinterpret_cast<ReadyJob*>(user);
    if (!job) return;
    job->ready = false;
    gShopDlg = nullptr;
    if (!Rebind(GetTickCount()) || !gShopDlgType || !gFindAll) return;
    void* arr = gFindAll(gShopDlgType, nullptr);
    if (!LooksLikeHeapPtr(arr)) return;
    const int n = static_cast<int>(
        *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18));
    for (int i = 0; i < n && i < 16; ++i) {
        void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                            static_cast<size_t>(i) * sizeof(void*));
        if (!LooksLikeHeapPtr(o)) continue;
        if (!ShopDlgLooksOpen(o)) continue;
        gShopDlg = o;
        job->ready = true;
        return;
    }
}

struct CloseJob {
    bool ok = false;
    bool wasReady = false;
    const char* err = "init";
};

void CloseJobOnMain(void* user) {
    auto* job = reinterpret_cast<CloseJob*>(user);
    if (!job) return;
    job->ok = false;
    job->wasReady = false;
    job->err = "init";
    __try {
        ReadyJob ready{};
        ReadyJobOnMain(&ready);
        job->wasReady = ready.ready;
        if (!ready.ready || !LooksLikeHeapPtr(gShopDlg)) {
            job->ok = true;
            job->err = "already-closed";
            return;
        }
        void* dlg = gShopDlg;
        BindUnityHelpers();
        auto* press = FnFromMi<FnButtonPress>(gMiButtonPress, kRvaButtonPress);
        auto* setRet = reinterpret_cast<FnShopSetRet>(
            gMiShopSetRet && gMiShopSetRet->methodPointer ? gMiShopSetRet->methodPointer
                                                         : AtRva<void*>(kRvaShopSetRet));
        auto* closeFn = reinterpret_cast<FnUiDialogClose>(
            gMiUiDialogClose && gMiUiDialogClose->methodPointer
                ? gMiUiDialogClose->methodPointer
                : AtRva<void*>(kRvaUiDialogClose));
        auto* getGo = FnFromMi<FnGetGameObject>(gMiGetGameObject, kRvaGetGameObject);
        auto* setActive = FnFromMi<FnGoSetActive>(gMiGoSetActive, kRvaGoSetActive);

        // 1) 官方退出钮：Awake 把 buttonExit.onClick → SetRet
        void* btExit = ReadPtr(dlg, kOffShopButtonExit);
        if (press && UnityAlive(btExit)) {
            __try {
                press(btExit, gMiButtonPress);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        // 2) 直调 SetRet（无 listener 时兜底）
        if (setRet) {
            __try {
                setRet(dlg, gMiShopSetRet);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ReadyJob afterSet{};
        ReadyJobOnMain(&afterSet);
        if (!afterSet.ready) {
            gShopDlg = nullptr;
            job->ok = true;
            job->err = "ok-exit";
            return;
        }
        dlg = LooksLikeHeapPtr(gShopDlg) ? gShopDlg : dlg;
        // 3) UIDialog.Close
        if (closeFn && LooksLikeHeapPtr(dlg)) {
            __try {
                closeFn(dlg, gMiUiDialogClose);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        ReadyJob afterClose{};
        ReadyJobOnMain(&afterClose);
        if (!afterClose.ready) {
            gShopDlg = nullptr;
            job->ok = true;
            job->err = "ok-close";
            return;
        }
        // 4) 仍挡操作：强拆 GameObject.active（BIN：Close 后 FindAll 残留 + 角色锁操作）
        dlg = LooksLikeHeapPtr(gShopDlg) ? gShopDlg : dlg;
        if (getGo && setActive && LooksLikeHeapPtr(dlg)) {
            void* go = nullptr;
            __try {
                go = getGo(dlg, gMiGetGameObject);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                go = nullptr;
            }
            if (go) {
                __try {
                    setActive(go, false, gMiGoSetActive);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
        ReadyJob afterForce{};
        ReadyJobOnMain(&afterForce);
        gShopDlg = afterForce.ready ? gShopDlg : nullptr;
        job->ok = !afterForce.ready;
        job->err = afterForce.ready ? "still-open" : "ok-force-inactive";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->ok = false;
        job->err = "SEH";
    }
}

bool ReadPos2(void* actor, float& x, float& y) {
    x = y = 0.f;
    if (!LooksLikeHeapPtr(actor)) return false;
    __try {
        x = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(actor) + kOffActorPos);
        y = *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(actor) + kOffActorPos + 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LooksLikeNpcPool(void* cand) {
    if (!LooksLikeHeapPtr(cand)) return false;
    if (gNpcPoolKlass) {
        void* k = ReadPtr(cand, 0);
        if (k != gNpcPoolKlass) return false;
    }
    // _npcList @0x10：可空（刚进图）；有则须像托管 List
    void* list = ReadPtr(cand, kOffNpcPoolList);
    if (!list) return true;
    return LooksLikeHeapPtr(list);
}

void* ResolveNpcPoolOnMain() {
    if (gNpcPool && LooksLikeNpcPool(gNpcPool) && UnityAlive(gNpcPool)) return gNpcPool;
    gNpcPool = nullptr;
    if (!gNpcPoolKlass) gNpcPoolKlass = FindClass(kNpcPoolClass);
    if (!gNpcPoolKlass) {
        x::runtime::LogWThrottled(70, 5000, "Shop", "NpcPool klass miss hash=%s", kNpcPoolClass);
        return nullptr;
    }

    const auto& e = x::runtime::il2cpp::Get();
    auto classInit = [&](void* k) {
        if (!k || !e.runtimeClassInit) return;
        __try {
            e.runtimeClassInit(k);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    };
    auto staticsOf = [&](void* k) -> void* {
        if (!k || !e.classStaticData) return nullptr;
        __try {
            void* sd = e.classStaticData(k);
            return LooksLikeHeapPtr(sd) ? sd : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    };

    void* parent = nullptr;
    if (e.classParent) {
        __try {
            parent = e.classParent(gNpcPoolKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
    }
    classInit(gNpcPoolKlass);
    if (parent) classInit(parent);

    // 注意：ResolveSingleton() 内 LooksLikeNm，只适合 NetworkManager，不能复用。
    void* statics = staticsOf(parent);
    if (!statics) statics = staticsOf(gNpcPoolKlass);
    if (!statics) {
        x::runtime::LogWThrottled(71, 5000, "Shop", "NpcPool statics miss klass=%p parent=%p",
                                  gNpcPoolKlass, parent);
        return nullptr;
    }

    void* best = nullptr;
    for (size_t s = 0; s < 4; ++s) {
        void* lazy = ReadPtr(statics, s * sizeof(void*));
        void* cand = TryLazyValue(lazy);
        if (!cand) cand = lazy;
        if (!LooksLikeNpcPool(cand)) continue;
        if (!UnityAlive(cand)) continue;
        best = cand;
        break;
    }
    if (!best) {
        x::runtime::LogWThrottled(72, 5000, "Shop",
                                  "NpcPool singleton miss statics=%p (was using LooksLikeNm by bug)",
                                  statics);
        return nullptr;
    }
    gNpcPool = best;
    return gNpcPool;
}

struct TalkJob {
    float maxDist = kDefaultTalkDist;
    int preferTemplateId = 0;
    bool ok = false;
    int npcOid = 0;
    int matchedTpl = 0;
    float dist = 0.f;
    const char* err = "?";
};

int ReadNpcTemplateId(void* npc) {
    void* data = ReadPtr(npc, kOffNpcData);
    if (!LooksLikeHeapPtr(data)) return 0;
    return ReadI32(data, kOffNpcDataId);
}

bool ReadIl2CppStringUtf8(void* str, char* out, size_t outCap) {
    out[0] = 0;
    if (!str || outCap < 2) return false;
    __try {
        const int len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const auto* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        size_t n = 0;
        for (int i = 0; i < len && n + 1 < outCap; ++i) {
            const wchar_t c = chars[i];
            if (c < 0x80) {
                out[n++] = static_cast<char>(c);
            } else if (c < 0x800 && n + 2 < outCap) {
                out[n++] = static_cast<char>(0xC0 | (c >> 6));
                out[n++] = static_cast<char>(0x80 | (c & 0x3F));
            } else if (n + 3 < outCap) {
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

bool MenuTextLooksLikeShop(const char* utf8) {
    if (!utf8 || !utf8[0]) return false;
    // 繁中常见：商店 / 交易 / 買賣 / 買東西 / 賣東西 / 雜貨；简中兜底
    static const char* kKeys[] = {"商店", "交易", "買賣", "买卖", "買東西", "卖东西", "賣東西",
                                  "雜貨", "杂货", "Shop", "shop", nullptr};
    for (int i = 0; kKeys[i]; ++i) {
        if (strstr(utf8, kKeys[i])) return true;
    }
    return false;
}

void TalkJobOnMain(void* user) {
    auto* job = reinterpret_cast<TalkJob*>(user);
    if (!job) return;
    job->ok = false;
    job->npcOid = 0;
    job->matchedTpl = 0;
    job->dist = 0.f;
    job->err = "init";

    if (!ResolveApi() || !gGA) {
        job->err = "no GA";
        return;
    }

    void* wm = world::GetWorldManager();
    void* localUser = wm ? ReadPtr(wm, 0x28) : nullptr;  // WorldManager.MyUser
    if (!UnityAlive(localUser)) {
        job->err = "no LocalUser";
        return;
    }

    float px = 0.f, py = 0.f;
    if (!ReadPos2(localUser, px, py)) {
        job->err = "no pos";
        return;
    }

    void* pool = ResolveNpcPoolOnMain();
    if (!pool) {
        job->err = "no NpcPool";
        return;
    }
    void* list = ReadPtr(pool, kOffNpcPoolList);
    if (!LooksLikeHeapPtr(list)) {
        job->err = "no npcList";
        return;
    }

    const float maxD = job->maxDist > 1.f ? job->maxDist : kDefaultTalkDist;
    const float maxD2 = maxD * maxD;
    // 经典版 NPC 对话本身可远距开店（玩家手玩也不必贴脸）。指定模板时全图找该 tpl，
    // 勿用 tight 距离门禁——BIN 误杀后改 300 导致店图内 no target。
    // 无模板时仍用 maxD，避免乱点远处无关 NPC。
    constexpr float kTplMapWide = 8000.f;
    const float tplMaxD2 =
        job->preferTemplateId > 0 ? (kTplMapWide * kTplMapWide) : maxD2;
    const int n = ListSize(list);
    void* bestNear = nullptr;
    float bestNearD2 = maxD2;
    int bestNearOid = 0;
    int bestNearTpl = 0;
    void* bestTpl = nullptr;
    float bestTplD2 = tplMaxD2;
    int bestTplOid = 0;
    for (int i = 0; i < n && i < 256; ++i) {
        void* npc = ListAt(list, i);
        if (!UnityAlive(npc)) continue;
        const int oid = ReadI32(npc, kOffNpcObjectId);
        if (oid <= 0) continue;
        float nx = 0.f, ny = 0.f;
        if (!ReadPos2(npc, nx, ny)) continue;
        const float dx = nx - px;
        const float dy = ny - py;
        const float d2 = dx * dx + dy * dy;
        const int tpl = ReadNpcTemplateId(npc);
        if (d2 < bestNearD2) {
            bestNearD2 = d2;
            bestNear = npc;
            bestNearOid = oid;
            bestNearTpl = tpl;
        }
        if (job->preferTemplateId > 0 && tpl == job->preferTemplateId && d2 < bestTplD2) {
            bestTplD2 = d2;
            bestTpl = npc;
            bestTplOid = oid;
        }
    }

    void* best = bestTpl ? bestTpl : bestNear;
    float bestD2 = bestTpl ? bestTplD2 : bestNearD2;
    int bestOid = bestTpl ? bestTplOid : bestNearOid;
    int bestTplId = bestTpl ? job->preferTemplateId : bestNearTpl;
    if (!best) {
        job->err = job->preferTemplateId > 0 ? "no target npc" : "no nearby npc";
        return;
    }

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    void* ulKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    // void(Npc*) 在 UL 上不唯一 → 哈希主路径；kind 只验。
    constexpr MethodShape kTalk{1, TypeKind::Void, true, true, {TypeKind::Ptr}};
    MethodInfoHead* miTalk =
        ResolveMi(ulKlass, kRvaUserLocalTalkToNpc, kTalk, "TalkToNpc", kHashTalkToNpc);
    auto fn = miTalk && miTalk->methodPointer
                  ? reinterpret_cast<FnTalkToNpc>(miTalk->methodPointer)
                  : AtRva<FnTalkToNpc>(kRvaUserLocalTalkToNpc);
    if (!fn) {
        job->err = "no TalkToNpc";
        return;
    }
    __try {
        fn(localUser, best, miTalk);
        job->ok = true;
        job->npcOid = bestOid;
        job->matchedTpl = bestTplId;
        job->dist = std::sqrt(bestD2);
        job->err = "ok";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->ok = false;
        job->err = "SEH";
    }
}

struct FuncKeyTalkJob {
    bool ok = false;
    const char* err = "?";
};

void FuncKeyTalkJobOnMain(void* user) {
    auto* job = reinterpret_cast<FuncKeyTalkJob*>(user);
    if (!job) return;
    job->ok = false;
    job->err = "init";
    if (!ResolveApi() || !gGA) {
        job->err = "no GA";
        return;
    }
    void* wm = world::GetWorldManager();
    void* localUser = wm ? ReadPtr(wm, 0x28) : nullptr;
    if (!UnityAlive(localUser)) {
        job->err = "no LocalUser";
        return;
    }
    const auto& e = x::runtime::il2cpp::Get();
    void* klass = FindClass(kFuncKeyClass);
    if (!klass || !e.objectNew) {
        job->err = "no FuncKey klass";
        return;
    }
    void* fk = nullptr;
    __try {
        fk = e.objectNew(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fk = nullptr;
    }
    if (!LooksLikeHeapPtr(fk)) {
        job->err = "FuncKey alloc";
        return;
    }
    uint32_t gc = 0;
    if (e.gcHandleNew) {
        __try {
            gc = e.gcHandleNew(fk, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            gc = 0;
        }
    }
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    // .ctor(FuncType,int) 唯一；OnFuncKey void(3) 在 UL 上唯一。
    constexpr MethodShape kCtor{2, TypeKind::Void, true, false, {TypeKind::Any, TypeKind::I32}};
    MethodInfoHead* miCtor = ResolveMi(klass, kRvaFuncKeyCtor, kCtor, ".ctor", nullptr);
    void* ulKlass = x::runtime::il2cpp_shape::ResolveUserLocalKlass();
    constexpr MethodShape kFk{3,
                              TypeKind::Void,
                              true,
                              true,
                              {TypeKind::I32, TypeKind::Ptr, TypeKind::U32}};
    MethodInfoHead* miFk =
        ResolveMi(ulKlass, kRvaOnFuncKey, kFk, "OnFuncKey", kHashOnFuncKey);
    auto ctor = miCtor && miCtor->methodPointer
                    ? reinterpret_cast<FnFuncKeyCtor>(miCtor->methodPointer)
                    : AtRva<FnFuncKeyCtor>(kRvaFuncKeyCtor);
    auto onFk = miFk && miFk->methodPointer ? reinterpret_cast<FnOnFuncKey>(miFk->methodPointer)
                                            : AtRva<FnOnFuncKey>(kRvaOnFuncKey);
    if (!ctor || !onFk) {
        if (gc && e.gcHandleFree) e.gcHandleFree(gc);
        job->err = "no OnFuncKey";
        return;
    }
    __try {
        ctor(fk, kFuncTypeBasicAction, kFkmBasicActionNpcTalk, miCtor);
        onFk(localUser, kKeyInputDown, fk, 0u, miFk);
        onFk(localUser, kKeyInputUp, fk, 0u, miFk);
        job->ok = true;
        job->err = "ok";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->ok = false;
        job->err = "SEH";
    }
    if (gc && e.gcHandleFree) {
        __try {
            e.gcHandleFree(gc);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

struct ScriptMenuJob {
    bool ok = false;
    int dlgType = -1;
    int menuN = 0;
    int picked = -1;
    const char* err = "?";
    char pickedText[64]{};
};

void ScriptMenuJobOnMain(void* user) {
    auto* job = reinterpret_cast<ScriptMenuJob*>(user);
    if (!job) return;
    job->ok = false;
    job->dlgType = -1;
    job->menuN = 0;
    job->picked = -1;
    job->pickedText[0] = 0;
    job->err = "init";
    if (!ResolveApi() || !gGA || !gFindAll) {
        job->err = "no GA";
        return;
    }
    if (!gUiDlgKlass)
        gUiDlgKlass =
            x::runtime::il2cpp_prefab::FindClassCached(kUiUtilDialogExClass, kPrefabUtilDialogEx)
                .klass;
    if (!gUiDlgType && gUiDlgKlass) gUiDlgType = ClassTypeObject(gUiDlgKlass);
    if (!gUiDlgType) {
        job->err = "no UIUtilDialogEx";
        return;
    }
    void* arr = nullptr;
    __try {
        arr = gFindAll(gUiDlgType, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        arr = nullptr;
    }
    const int n = LooksLikeHeapPtr(arr)
                      ? static_cast<int>(
                            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + 0x18))
                      : 0;
    void* dlg = nullptr;
    for (int i = 0; i < n && i < 8; ++i) {
        void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + 0x20 +
                                            static_cast<size_t>(i) * sizeof(void*));
        if (!UnityAlive(o)) continue;
        dlg = o;
        break;
    }
    if (!dlg) {
        job->err = "no dialog";
        return;
    }
    job->dlgType = ReadI32(dlg, kOffUiDlgType);
    auto clickOk = AtRva<FnUiDlgOnClickOk>(kRvaUiDlgOnClickBtOk);
    auto selectMenu = AtRva<FnUiDlgSelectMenu>(kRvaUiDlgSelectMenu);
    // 明文 OnClickBtOk 在 dump 残留；菜单 void(int) 不唯一 → RVA 主路径。
    if (gUiDlgKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kOk{0, TypeKind::Void, true, false, {}};
        if (MethodInfoHead* miOk =
                ResolveMi(gUiDlgKlass, kRvaUiDlgOnClickBtOk, kOk, "OnClickBtOk", nullptr)) {
            if (miOk->methodPointer) clickOk = reinterpret_cast<FnUiDlgOnClickOk>(miOk->methodPointer);
        }
        constexpr MethodShape kMenu{1, TypeKind::Void, true, false, {TypeKind::I32}};
        if (MethodInfoHead* miMenu =
                ResolveMi(gUiDlgKlass, kRvaUiDlgSelectMenu, kMenu, "SetKeyFocus", nullptr)) {
            if (miMenu->methodPointer)
                selectMenu = reinterpret_cast<FnUiDlgSelectMenu>(miMenu->methodPointer);
        }
    }
    if (!clickOk) {
        job->err = "no OnClickBtOk";
        return;
    }

    if (job->dlgType == kUiDlgTypeList) {
        void* texts = ReadPtr(dlg, kOffUiDlgMenuTexts);
        const int mn = ListSize(texts);
        job->menuN = mn;
        int pick = -1;
        char buf[96]{};
        for (int i = 0; i < mn && i < 32; ++i) {
            void* s = ListAt(texts, i);
            if (!ReadIl2CppStringUtf8(s, buf, sizeof(buf))) continue;
            if (MenuTextLooksLikeShop(buf)) {
                pick = i;
                strncpy_s(job->pickedText, buf, _TRUNCATE);
                break;
            }
        }
        // 杂货 NPC 常把商店放在第 0 项；无关键词时兜底点 0
        if (pick < 0 && mn > 0) {
            pick = 0;
            if (ReadIl2CppStringUtf8(ListAt(texts, 0), buf, sizeof(buf)))
                strncpy_s(job->pickedText, buf, _TRUNCATE);
        }
        if (pick < 0) {
            job->err = "empty menu";
            return;
        }
        job->picked = pick;
        __try {
            if (selectMenu) selectMenu(dlg, pick, nullptr);
            clickOk(dlg, nullptr);
            job->ok = true;
            job->err = "ok";
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            job->ok = false;
            job->err = "SEH";
        }
        return;
    }

    // Say / YesNo：点确定推进（部分店会多段对话后才开店或出菜单）
    if (job->dlgType == kUiDlgTypeText || job->dlgType == kUiDlgTypeYesNo) {
        __try {
            clickOk(dlg, nullptr);
            job->ok = true;
            job->err = "ok-advance";
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            job->ok = false;
            job->err = "SEH";
        }
        return;
    }
    job->err = "bad type";
}

struct ScanJob {
    bool equip = false;
    BagItem* items = nullptr;
    int maxItems = 0;
    int count = 0;
    bool ok = false;
};

void FillName(int itemId, char* out, size_t outCap) {
    if (!out || outCap == 0) return;
    out[0] = 0;
    if (itemId <= 0) return;
    char code[32]{};
    snprintf(code, sizeof(code), "%d", itemId);
    const char* nm =
        xcat::ItemCatalogLookupName(xcat::GetSharedItemCatalog(x::runtime::GetBinDir()), code);
    if (nm && nm[0]) strncpy_s(out, outCap, nm, _TRUNCATE);
}

void ScanJobOnMain(void* user) {
    auto* job = reinterpret_cast<ScanJob*>(user);
    if (!job || !job->items || job->maxItems <= 0) return;
    job->count = 0;
    job->ok = false;
    const int invType = job->equip ? kInvTiEquip : kInvTiEtc;
    void* list = GetBagList(invType);
    if (!list) return;
    const int n = ListSize(list);
    if (n <= 0 || n > 512) return;
    const bool oneBased = (n > 1 && ListAt(list, 0) == nullptr && ListAt(list, 1) != nullptr);
    for (int i = 0; i < n && job->count < job->maxItems; ++i) {
        void* slot = ListAt(list, i);
        if (!LooksLikeHeapPtr(slot)) continue;
        const int itemId = ReadI32(slot, kOffSlotItemId);
        if (itemId <= 0) continue;
        const int qty = ItemQty(slot);
        if (qty <= 0) continue;
        const int pos = oneBased ? i : (i + 1);
        if (pos <= 0) continue;

        BagItem& it = job->items[job->count++];
        it = {};
        it.pos = pos;
        it.itemId = itemId;
        it.count = qty;
        it.invType = invType;
        FillName(itemId, it.name, sizeof(it.name));
        // 可卖：优先离线 item_value 卖价；无名无价时仍尝试（表外物品交给服端拒）
        char code[32]{};
        snprintf(code, sizeof(code), "%d", itemId);
        const auto& pack = xcat::GetSharedItemCatalog(x::runtime::GetBinDir());
        const int price = xcat::ItemCatalogLookupSellPrice(pack, code);
        if (price > 0) {
            it.sellable = true;
        } else {
            // 无名无卖价 / 卖价0：NPC 卖栏通常不收（如 4161001 新手指南）
            it.sellable = false;
        }
    }
    job->ok = true;
}

struct SellJob {
    int invType = 0;
    int pos = 0;
    int itemId = 0;
    int count = 0;
    bool ok = false;
    char err[64]{};
};

// 卖栏列表 = 开店后客户端从背包投影的可卖槽（不是「店专属商品表」）。
// 优先 itemId+pos，其次同 itemId 任一格。
bool FindSellListIndex(void* dlg, int itemId, int preferPos, int& outIndex, int& outPos,
                       int& outListN) {
    outIndex = -1;
    outPos = 0;
    outListN = 0;
    if (!LooksLikeHeapPtr(dlg) || itemId <= 0) return false;
    void* list = ReadPtr(dlg, kOffSellItemList);
    const int n = ListSize(list);
    outListN = n;
    if (n <= 0 || n > 512) return false;
    int fallback = -1;
    int fallbackPos = 0;
    for (int i = 0; i < n; ++i) {
        void* it = ListAt(list, i);
        if (!LooksLikeHeapPtr(it)) continue;
        if (ReadI32(it, kOffShopItemId) != itemId) continue;
        const int p = ReadI32(it, kOffShopItemPos);
        if (preferPos > 0 && p == preferPos) {
            outIndex = i;
            outPos = p;
            return true;
        }
        if (fallback < 0) {
            fallback = i;
            fallbackPos = p;
        }
    }
    if (fallback < 0) return false;
    outIndex = fallback;
    outPos = fallbackPos;
    return true;
}

// ItemType Equip=1..Etc=4 → 角色区 UITab 下标 0..3（BIN：错 TAB 时 _sellItemList 为空）
int InvTypeToCharTabIndex(int invType) {
    if (invType >= 1 && invType <= 5) return invType - 1;
    return 0;
}

int UiTabItemCount(void* tab) {
    if (!LooksLikeHeapPtr(tab)) return 0;
    return ListSize(ReadPtr(tab, kOffUiTabItems));
}

void* PickShopCharInvTab(void* dlg, int wantIdx) {
    if (!LooksLikeHeapPtr(dlg)) return nullptr;
    void* t1 = ReadPtr(dlg, kOffShopUiTab1);  // 优先角色区
    void* t0 = ReadPtr(dlg, kOffShopUiTab0);
    const int n1 = UiTabItemCount(t1);
    const int n0 = UiTabItemCount(t0);
    if (n1 > wantIdx && n1 >= 4) return t1;
    if (n0 > wantIdx && n0 >= 4) return t0;
    if (n1 > wantIdx) return t1;
    if (n0 > wantIdx) return t0;
    if (LooksLikeHeapPtr(t1)) return t1;
    if (LooksLikeHeapPtr(t0)) return t0;
    return nullptr;
}

// 卖出前切到对应背包 TAB，否则 CmpSellItem 投影列表为空 → LIST_MISS。
// outSwitched：本拍确实调用了 OnClickTab（同帧列表可能尚未刷新，调用方应 LIST_STALE 重试）。
bool EnsureShopSellInvTab(void* dlg, int invType, bool* outSwitched) {
    if (outSwitched) *outSwitched = false;
    const int want = InvTypeToCharTabIndex(invType);
    void* tab = PickShopCharInvTab(dlg, want);
    if (!LooksLikeHeapPtr(tab)) return false;
    const int cur = ReadI32(tab, kOffUiTabCurrentIndex);
    if (cur == want) return true;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    // UITab.OnClickTab(int) — void(int) 不唯一 → 哈希；读 klass from object[0]。
    void* tabKlass = nullptr;
    __try {
        tabKlass = *reinterpret_cast<void**>(tab);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        tabKlass = nullptr;
    }
    constexpr MethodShape kTab{1, TypeKind::Void, true, true, {TypeKind::I32}};
    MethodInfoHead* miTab =
        ResolveMi(tabKlass, kRvaUiTabOnClickTab, kTab, "OnClickTab", kHashUiTabOnClick);
    auto onClick = miTab && miTab->methodPointer
                       ? reinterpret_cast<FnUiTabOnClickTab>(miTab->methodPointer)
                       : AtRva<FnUiTabOnClickTab>(kRvaUiTabOnClickTab);
    if (!onClick) return false;
    __try {
        onClick(tab, want, miTab);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (outSwitched) *outSwitched = true;
    x::runtime::LogI("Shop", "sell tab switch inv=%d tab %d→%d (UITab.OnClickTab)", invType, cur,
                     want);
    return true;
}

void SellJobOnMain(void* user) {
    auto* job = reinterpret_cast<SellJob*>(user);
    if (!job) return;
    job->ok = false;
    job->err[0] = 0;
    __try {
        gLastRebindMs = 0;
        if (!Rebind(GetTickCount())) {
            strncpy_s(job->err, "UNBOUND", _TRUNCATE);
            return;
        }
        ReadyJob ready{};
        ReadyJobOnMain(&ready);
        if (!ready.ready || !LooksLikeHeapPtr(gShopDlg)) {
            strncpy_s(job->err, "NO_SHOP", _TRUNCATE);
            return;
        }
        auto* sendPkt = reinterpret_cast<FnSendSellPacket>(
            gMiSendSellPacket && gMiSendSellPacket->methodPointer
                ? gMiSendSellPacket->methodPointer
                : AtRva<void*>(kRvaSendSellRequestPacket));
        auto* cmpSell = reinterpret_cast<FnCmpSellItem>(
            gMiCmpSellItem && gMiCmpSellItem->methodPointer ? gMiCmpSellItem->methodPointer
                                                           : AtRva<void*>(kRvaCmpSellItem));
        if (!sendPkt) {
            strncpy_s(job->err, "NO_RPC", _TRUNCATE);
            return;
        }
        if (job->itemId <= 0) {
            snprintf(job->err, sizeof(job->err), "BAD_ARGS id=%d", job->itemId);
            return;
        }
        // 上一笔 UI 请求未清：立刻返回，由 sellbag 步进重试（禁止在主线程 Sleep）
        if ((ReadI32(gShopDlg, kOffHasShopRequestSent) & 0xFF) != 0) {
            strncpy_s(job->err, "SHOP_BUSY", _TRUNCATE);
            return;
        }
        // 角色区 TAB 必须对齐 invType，否则卖栏投影为空（BIN: sellListN=0 → LIST_MISS）
        bool tabSwitched = false;
        (void)EnsureShopSellInvTab(gShopDlg, job->invType, &tabSwitched);
        // 开店语义：卖栏=背包可卖投影。先 CmpSellItem 刷新，再按下标走 UI 发包。
        if (cmpSell) {
            __try {
                cmpSell(gShopDlg, gMiCmpSellItem);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        int sellIdx = -1;
        int sellPos = 0;
        int listN = 0;
        if (!FindSellListIndex(gShopDlg, job->itemId, job->pos, sellIdx, sellPos, listN)) {
            // 刚切 TAB / 投影仍空：同帧列表可能未刷新 → 交 sellbag 下步重试，勿记失败
            if (tabSwitched || listN <= 0) {
                strncpy_s(job->err, "LIST_STALE", _TRUNCATE);
                x::runtime::LogW("Shop",
                                 "sell LIST_STALE id=%d bagPos=%d sellListN=%d switched=%d (retry)",
                                 job->itemId, job->pos, listN, tabSwitched ? 1 : 0);
                return;
            }
            snprintf(job->err, sizeof(job->err), "LIST_MISS id=%d pos=%d n=%d", job->itemId,
                     job->pos, listN);
            x::runtime::LogW(
                "Shop",
                "sell LIST_MISS id=%d bagPos=%d sellListN=%d (开店可卖；未在 "
                "_sellItemList@0x198 命中)",
                job->itemId, job->pos, listN);
            return;
        }
        WriteI32(gShopDlg, kOffSellSelectedIndex, sellIdx);
        WriteI32(gShopDlg, kOffLastSellIndex, sellIdx);
        int qty = job->count > 0 ? job->count : 1;
        // 装备栏 BundleNumber 常非堆叠数；BIN 曾 qty=7 卖弓后 135ms→Disconnected/205
        if (job->invType == 1 /* Equip */) qty = 1;
        sendPkt(gShopDlg, qty, gMiSendSellPacket);
        job->ok = true;
        snprintf(job->err, sizeof(job->err), "FIRED via=ui");
        x::runtime::LogI(
            "Shop",
            "sell FIRED via=ui inv=%d bagPos=%d shopPos=%d idx=%d id=%d cnt=%d listN=%d",
            job->invType, job->pos, sellPos, sellIdx, job->itemId, qty, listN);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strncpy_s(job->err, "EXCEPTION", _TRUNCATE);
    }
}

// 买栏：+0x178 / +0x180（UITab 二选一）。产品两侧都扫，命中即用该表下标。
bool FindBuyListIndex(void* dlg, int itemId, int& outIndex, int& outPrice, int& outListN,
                      size_t& outListOff) {
    outIndex = -1;
    outPrice = 0;
    outListN = 0;
    outListOff = 0;
    if (!LooksLikeHeapPtr(dlg) || itemId <= 0) return false;
    const size_t offs[] = {kOffBuyItemList0, kOffBuyItemList1};
    for (size_t lo : offs) {
        void* list = ReadPtr(dlg, lo);
        const int n = ListSize(list);
        outListN += n > 0 ? n : 0;
        if (n <= 0 || n > 512) continue;
        for (int i = 0; i < n; ++i) {
            void* it = ListAt(list, i);
            if (!LooksLikeHeapPtr(it)) continue;
            if (ReadI32(it, kOffShopItemId) != itemId) continue;
            outIndex = i;
            outPrice = ReadI32(it, kOffShopItemPrice);
            outListOff = lo;
            return true;
        }
    }
    return false;
}

struct BuyJob {
    int itemId = 0;
    int count = 0;
    bool ok = false;
    char err[64]{};
};

void BuyJobOnMain(void* user) {
    auto* job = reinterpret_cast<BuyJob*>(user);
    if (!job) return;
    job->ok = false;
    job->err[0] = 0;
    __try {
        gLastRebindMs = 0;
        if (!Rebind(GetTickCount())) {
            strncpy_s(job->err, "UNBOUND", _TRUNCATE);
            return;
        }
        ReadyJob ready{};
        ReadyJobOnMain(&ready);
        if (!ready.ready || !LooksLikeHeapPtr(gShopDlg)) {
            strncpy_s(job->err, "NO_SHOP", _TRUNCATE);
            return;
        }
        auto* sendPkt = reinterpret_cast<FnSendBuyPacket>(
            gMiSendBuyPacket && gMiSendBuyPacket->methodPointer
                ? gMiSendBuyPacket->methodPointer
                : AtRva<void*>(kRvaSendBuyRequestPacket));
        if (!sendPkt) {
            strncpy_s(job->err, "NO_RPC", _TRUNCATE);
            return;
        }
        if (job->itemId <= 0) {
            snprintf(job->err, sizeof(job->err), "BAD_ARGS id=%d", job->itemId);
            return;
        }
        if ((ReadI32(gShopDlg, kOffHasShopRequestSent) & 0xFF) != 0) {
            strncpy_s(job->err, "SHOP_BUSY", _TRUNCATE);
            return;
        }
        int buyIdx = -1;
        int price = 0;
        int listN = 0;
        size_t listOff = 0;
        if (!FindBuyListIndex(gShopDlg, job->itemId, buyIdx, price, listN, listOff)) {
            snprintf(job->err, sizeof(job->err), "LIST_MISS id=%d n=%d", job->itemId, listN);
            x::runtime::LogW("Shop", "buy LIST_MISS id=%d buyListN=%d", job->itemId, listN);
            return;
        }
        const int qty = job->count > 0 ? job->count : 1;
        if (price > 0) {
            int64_t meso = -1;
            void* wm = world::GetWorldManager();
            void* cd = wm ? ReadPtr(wm, kOffWmCharacterData) : nullptr;
            void* stat = LooksLikeHeapPtr(cd) ? ReadPtr(cd, kOffCdCharacterStat) : nullptr;
            if (LooksLikeHeapPtr(stat)) {
                __try {
                    meso = *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(stat) +
                                                       kOffCsMoney);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    meso = -1;
                }
            }
            if (meso >= 0 && (int64_t)price * (int64_t)qty > meso) {
                strncpy_s(job->err, "NO_MESO", _TRUNCATE);
                return;
            }
        }
        WriteI32(gShopDlg, kOffBuySelectedIndex, buyIdx);
        WriteI32(gShopDlg, kOffLastBuyIndex, buyIdx);
        sendPkt(gShopDlg, qty, gMiSendBuyPacket);
        job->ok = true;
        snprintf(job->err, sizeof(job->err), "FIRED via=ui");
        x::runtime::LogI("Shop",
                         "buy FIRED via=ui id=%d idx=%d cnt=%d price=%d listOff=0x%zX listN=%d",
                         job->itemId, buyIdx, qty, price, listOff, listN);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strncpy_s(job->err, "EXCEPTION", _TRUNCATE);
    }
}

struct BuyOfferJob {
    int itemId = 0;
    bool inShop = false;
    int price = 0;
    bool ok = false;
};

void BuyOfferJobOnMain(void* user) {
    auto* job = reinterpret_cast<BuyOfferJob*>(user);
    if (!job) return;
    job->ok = false;
    job->inShop = false;
    job->price = 0;
    gLastRebindMs = 0;
    if (!Rebind(GetTickCount())) return;
    ReadyJob ready{};
    ReadyJobOnMain(&ready);
    if (!ready.ready || !LooksLikeHeapPtr(gShopDlg)) {
        job->ok = true;
        return;
    }
    int idx = -1;
    int price = 0;
    int listN = 0;
    size_t listOff = 0;
    if (FindBuyListIndex(gShopDlg, job->itemId, idx, price, listN, listOff)) {
        job->inShop = true;
        job->price = price;
    }
    job->ok = true;
}

struct PresentJob {
    int invType = 0;
    int itemId = 0;
    bool present = false;
    int count = 0;
    bool ok = false;
};

void PresentJobOnMain(void* user) {
    auto* job = reinterpret_cast<PresentJob*>(user);
    if (!job) return;
    job->ok = false;
    job->present = false;
    job->count = 0;
    void* list = GetBagList(job->invType);
    if (!list) return;
    const int n = ListSize(list);
    int total = 0;
    bool found = false;
    for (int i = 0; i < n && i < 512; ++i) {
        void* slot = ListAt(list, i);
        if (!LooksLikeHeapPtr(slot)) continue;
        if (ReadI32(slot, kOffSlotItemId) != job->itemId) continue;
        found = true;
        total += ItemQty(slot);
    }
    job->present = found && total > 0;
    job->count = total;
    job->ok = true;
}

struct MesoJob {
    int64_t meso = -1;
};

void MesoJobOnMain(void* user) {
    auto* job = reinterpret_cast<MesoJob*>(user);
    if (!job) return;
    job->meso = -1;
    void* wm = world::GetWorldManager();
    if (!wm) return;
    void* cd = ReadPtr(wm, kOffWmCharacterData);
    void* stat = LooksLikeHeapPtr(cd) ? ReadPtr(cd, kOffCdCharacterStat) : nullptr;
    if (!LooksLikeHeapPtr(stat)) return;
    __try {
        job->meso = *reinterpret_cast<int64_t*>(reinterpret_cast<uint8_t*>(stat) + kOffCsMoney);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->meso = -1;
    }
}

struct UsageJob {
    bool equip = false;
    int used = 0;
    int cap = 0;
    bool ok = false;
};

void UsageJobOnMain(void* user) {
    auto* job = reinterpret_cast<UsageJob*>(user);
    if (!job) return;
    job->ok = false;
    job->used = 0;
    job->cap = 0;
    const int invType = job->equip ? kInvTiEquip : kInvTiEtc;
    void* list = GetBagList(invType);
    if (!list) return;
    const int n = ListSize(list);
    if (n <= 0 || n > 512) return;
    job->cap = n;
    int used = 0;
    for (int i = 0; i < n; ++i) {
        void* slot = ListAt(list, i);
        if (!LooksLikeHeapPtr(slot)) continue;
        if (ReadI32(slot, kOffSlotItemId) > 0) ++used;
    }
    job->used = used;
    job->ok = true;
}

}  // namespace

bool EnsureBound() { return Rebind(GetTickCount()); }

bool ShopReady(bool& outReady) {
    outReady = false;
    ReadyJob job{};
    if (!x::runtime::managed_main::Call(&ReadyJobOnMain, &job, kJobWaitMs)) return false;
    outReady = job.ready;
    return true;
}

bool CloseShop() {
    CloseJob job{};
    if (!x::runtime::managed_main::Call(&CloseJobOnMain, &job, kJobWaitMs)) return false;
    static DWORD sLastCloseLogMs = 0;
    const DWORD now = GetTickCount();
    if (now - sLastCloseLogMs > 800 || !job.ok ||
        (job.err && strncmp(job.err, "ok", 2) != 0 && strcmp(job.err, "already-closed") != 0)) {
        sLastCloseLogMs = now;
        x::runtime::LogI("Shop", "CloseShop ok=%d wasReady=%d err=%s", job.ok ? 1 : 0,
                         job.wasReady ? 1 : 0, job.err ? job.err : "?");
    }
    return job.ok;
}

bool TryTalkNearestNpc(float maxDist, int preferTemplateId) {
    TalkJob job{};
    job.maxDist = maxDist > 1.f ? maxDist : kDefaultTalkDist;
    job.preferTemplateId = preferTemplateId;
    if (!x::runtime::managed_main::Call(&TalkJobOnMain, &job, kJobWaitMs)) return false;
    const DWORD now = GetTickCount();
    if (now - gLastTalkLogMs > 2500) {
        gLastTalkLogMs = now;
        x::runtime::LogI("Shop", "TryTalkNearest ok=%d oid=%d tpl=%d want=%d dist=%.1f err=%s",
                         job.ok ? 1 : 0, job.npcOid, job.matchedTpl, preferTemplateId, job.dist,
                         job.err ? job.err : "?");
    }
    return job.ok;
}

bool TryNpcTalkFuncKey() {
    FuncKeyTalkJob job{};
    if (!x::runtime::managed_main::Call(&FuncKeyTalkJobOnMain, &job, kJobWaitMs)) return false;
    const DWORD now = GetTickCount();
    if (now - gLastFuncKeyLogMs > 2500) {
        gLastFuncKeyLogMs = now;
        x::runtime::LogI("Shop", "TryNpcTalkFuncKey ok=%d err=%s", job.ok ? 1 : 0,
                         job.err ? job.err : "?");
    }
    return job.ok;
}

bool TryConfirmShopScriptMenu() {
    ScriptMenuJob job{};
    if (!x::runtime::managed_main::Call(&ScriptMenuJobOnMain, &job, kJobWaitMs)) return false;
    const DWORD now = GetTickCount();
    if (job.ok || (job.err && strcmp(job.err, "no dialog") != 0)) {
        if (now - gLastMenuLogMs > 1500) {
            gLastMenuLogMs = now;
            x::runtime::LogI("Shop",
                             "TryConfirmShopScriptMenu ok=%d type=%d menuN=%d pick=%d text=%s err=%s",
                             job.ok ? 1 : 0, job.dlgType, job.menuN, job.picked,
                             job.pickedText[0] ? job.pickedText : "-", job.err ? job.err : "?");
        }
    }
    return job.ok;
}

bool ScanBag(bool equipBag, BagItem* items, int maxItems, int& outCount) {
    outCount = 0;
    if (!items || maxItems <= 0) return false;
    ScanJob job{};
    job.equip = equipBag;
    job.items = items;
    job.maxItems = maxItems;
    if (!x::runtime::managed_main::Call(&ScanJobOnMain, &job, kJobWaitMs)) return false;
    outCount = job.count;
    return job.ok;
}

bool SellItem(int invType, int pos, int itemId, int count, std::string& outErr) {
    outErr.clear();
    SellJob job{};
    job.invType = invType;
    job.pos = pos;
    job.itemId = itemId;
    job.count = count;
    if (!x::runtime::managed_main::Call(&SellJobOnMain, &job, kJobWaitMs)) {
        outErr = "MAIN_TIMEOUT";
        return false;
    }
    outErr = job.err;
    return job.ok;
}

bool BuyItem(int itemId, int count, std::string& outErr) {
    outErr.clear();
    BuyJob job{};
    job.itemId = itemId;
    job.count = count;
    if (!x::runtime::managed_main::Call(&BuyJobOnMain, &job, kJobWaitMs)) {
        outErr = "MAIN_TIMEOUT";
        return false;
    }
    outErr = job.err;
    return job.ok;
}

bool QueryShopBuyOffer(int itemId, bool& outInShop, int& outPrice) {
    outInShop = false;
    outPrice = 0;
    BuyOfferJob job{};
    job.itemId = itemId;
    if (!x::runtime::managed_main::Call(&BuyOfferJobOnMain, &job, kJobWaitMs)) return false;
    outInShop = job.inShop;
    outPrice = job.price;
    return job.ok;
}

bool QueryItemPresent(int invType, int itemId, bool& outPresent, int& outCount) {
    outPresent = false;
    outCount = 0;
    PresentJob job{};
    job.invType = invType;
    job.itemId = itemId;
    if (!x::runtime::managed_main::Call(&PresentJobOnMain, &job, kJobWaitMs)) return false;
    outPresent = job.present;
    outCount = job.count;
    return job.ok;
}

bool QueryBagUsage(bool equipBag, int& outUsed, int& outCap) {
    outUsed = 0;
    outCap = 0;
    UsageJob job{};
    job.equip = equipBag;
    if (!x::runtime::managed_main::Call(&UsageJobOnMain, &job, kJobWaitMs)) return false;
    outUsed = job.used;
    outCap = job.cap;
    return job.ok;
}

int64_t QueryMeso() {
    MesoJob job{};
    if (!x::runtime::managed_main::Call(&MesoJobOnMain, &job, 800)) return -1;
    return job.meso;
}

namespace {

struct GrocerySeed {
    char npcId[24]{};
    char mapId[16]{};
    uint32_t tags = 0;  // 1=sell 2=potion 4=feed
};

constexpr uint32_t kTagSell = 1u;
constexpr uint32_t kTagPotion = 2u;  // 表字段保留；寻店不再按 potion/feed 选型
constexpr uint32_t kTagFeed = 4u;

std::mutex gSeedMu;
std::vector<GrocerySeed> gSeeds;
bool gSeedsTried = false;

std::string JoinBin(const char* rel) {
    std::string out = x::runtime::GetBinDir() ? x::runtime::GetBinDir() : "";
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out += '\\';
    out += rel ? rel : "";
    return out;
}

uint32_t ParseTags(const char* tags) {
    uint32_t t = 0;
    if (!tags) return t;
    if (strstr(tags, "sell")) t |= kTagSell;
    if (strstr(tags, "potion")) t |= kTagPotion;
    if (strstr(tags, "feed")) t |= kTagFeed;
    if (t == 0) t = kTagSell | kTagPotion;
    return t;
}

void EnsureGrocerySeeds() {
    std::lock_guard<std::mutex> lock(gSeedMu);
    if (gSeedsTried) return;
    gSeedsTried = true;
    const std::string path = JoinBin("dataservice\\grocery_shop_npc.tsv");
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        x::runtime::LogW("Shop", "grocery seed missing: %s", path.c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        char npc[24]{}, map[16]{}, tags[64]{};
        if (sscanf_s(line.c_str(), "%23s %15s %63s", npc, (unsigned)sizeof(npc), map,
                     (unsigned)sizeof(map), tags, (unsigned)sizeof(tags)) < 2)
            continue;
        GrocerySeed s{};
        strncpy_s(s.npcId, npc, _TRUNCATE);
        strncpy_s(s.mapId, map, _TRUNCATE);
        s.tags = ParseTags(tags);
        gSeeds.push_back(s);
    }
    x::runtime::LogI("Shop", "grocery seed loaded n=%zu path=%s", gSeeds.size(), path.c_str());
}

bool MapEqualsLoose(const char* a, const char* b) {
    if (!a || !b || !a[0] || !b[0]) return false;
    if (_stricmp(a, b) == 0) return true;
    // trim leading zeros on numeric ids
    while (*a == '0' && a[1]) ++a;
    while (*b == '0' && b[1]) ++b;
    return _stricmp(a, b) == 0;
}

std::string CurrentMapForHops() {
    const int id = ports::travel::CurrentMapId();
    if (id > 0) {
        char buf[16]{};
        snprintf(buf, sizeof(buf), "%d", id);
        return buf;
    }
    return ports::travel::CurrentMapKey();
}

bool PickNearestShop(const char* excludeMap, std::string& outNpcId, std::string& outShopId,
                     std::string& outMapName, int& outMapId) {
    EnsureGrocerySeeds();
    outNpcId.clear();
    outShopId.clear();
    outMapName.clear();
    outMapId = 0;
    if (gSeeds.empty()) return false;

    const std::string cur = CurrentMapForHops();
    int bestScore = INT_MAX;
    const GrocerySeed* best = nullptr;

    for (const auto& s : gSeeds) {
        if (excludeMap && excludeMap[0] && MapEqualsLoose(s.mapId, excludeMap)) continue;
        int hops = 0;
        if (!cur.empty() && cur != "?") {
            hops = features::travel::PathHopCount(cur.c_str(), s.mapId);
            if (hops < 0) hops = 9999;  // unreachable → last resort
        }
        // hops 优先；同 hops 偏好 potion（杂货更常直接开店 / 菜单更短）
        const int score = hops * 10000 + ((s.tags & kTagPotion) ? 0 : 1000) +
                          ((s.tags & kTagSell) ? 0 : 10);
        if (score < bestScore) {
            bestScore = score;
            best = &s;
        }
    }
    if (!best) return false;
    outNpcId = best->npcId;
    outShopId = best->npcId;  // Classic 无独立 shopId；填 npc 便于日志
    outMapName = best->mapId;
    outMapId = atoi(best->mapId);
    const int hopsLog = bestScore / 10000;
    x::runtime::LogI("Shop", "ResolveShop nearest npc=%s map=%s hops=%d potion=%d", best->npcId,
                     best->mapId, hopsLog == 9999 ? -1 : hopsLog,
                     (best->tags & kTagPotion) ? 1 : 0);
    return true;
}

}  // namespace

bool ResolveShopNpcForSell(std::string& outNpcId, std::string& outShopId, std::string& outMapName,
                            int& outMapId, const char* excludeMapName) {
    return PickNearestShop(excludeMapName, outNpcId, outShopId, outMapName, outMapId);
}

bool ResolveShopNpcForSupply(const char* /*preferredItemCode*/, std::string& outNpcId,
                              std::string& outShopId, std::string& outMapName, int& outMapId,
                              const char* excludeMapName) {
    // 不按补给品选型：与 ForSell 相同（店内有货再买，无货跳过）。
    return ResolveShopNpcForSell(outNpcId, outShopId, outMapName, outMapId, excludeMapName);
}

bool RechargeShurikensInOpenShop(int& outCharged, int& outSkippedNoMeso, int& outSkippedOther,
                                 std::string& outErr) {
    outCharged = 0;
    outSkippedNoMeso = 0;
    outSkippedOther = 0;
    outErr = "NOT_IMPL";
    x::runtime::LogW("Shop", "RechargeShurikensInOpenShop NOT_IMPL (UIShop Charge CF-flat)");
    return true;  // soft-skip for trip orchestration
}

}  // namespace x::features::ports::shop
