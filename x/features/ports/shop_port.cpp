// Classic TWMS shop_port — UIShopDialog ready + TalkToNpc + UI 买卖 / Charge。
// 字段：EnsureShopFieldOffsets（hash/明文 → field_get_offset；dump 常量 fallback）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "shop_port.h"

#include "travel_port.h"
#include "world_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_network.h"
#include "../../runtime/il2cpp_shape.h"
#include "../../runtime/il2cpp_prefab.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/anchor_lamps.h"
#include "../../ui/player_vitals.h"
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
#include <unordered_map>
#include <vector>

namespace x::features::ports::shop {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// UIShopDialog — Prefab TypeDef 434 · remounted 2026-08-06
constexpr char kUiShopDialogClass[] =
    "b8857fa084ceebc5620095cd12695b5f0e7ea3b1aabe4b5edbbfb0e0bc3ce54";
constexpr char kPrefabShopDialog[] = "UIShopDialog";
// NpcPool — remounted 2026-08-06（形：List@10 Dict@18 List@20 List@28 Field@30 int@38）
constexpr char kNpcPoolClass[] =
    "eedcd93edb42c78ca6c9aab2fdf52af5dec14c54da48aa00dcee5dba45a05d0";
// UIUtilDialogEx（脚本对话 / AskMenu）— Prefab TypeDefIndex 609
constexpr char kUiUtilDialogExClass[] =
    "c0e2575bfaabf8fa25bee32fa3d0b6972a771b99104acbf9f0c98590c225be3";
constexpr char kPrefabUtilDialogEx[] = "UIUtilDialogEx";
constexpr char kFuncKeyClass[] =
    "ad101c284c26e938b1f94c2666b42085d4958fe72cd8160af489f17a42a6f63";

// OutPacket SEND 13775 / Session Send — 与 travel_port 同源 · remounted 2026-08-06
constexpr uint32_t kRvaOutPacketCreate = 0x1CEEFC0;
constexpr uint32_t kRvaOutPacketEncode1Byte = 0x1CFB5A0;  // Encode1(sbyte)
constexpr uint32_t kRvaOutPacketEncode2Short = 0x1CFB8C0;
constexpr uint32_t kRvaOutPacketEncode4Int = 0x1CFB9D0;
constexpr uint32_t kRvaNmSend = 0x1CF0BD0;  // Session.SendPacket bool(OutPacket)
constexpr uint32_t kRvaUserLocalTalkToNpc = 0x10a80c0;  // remounted 2026-08-06
constexpr uint32_t kRvaOnFuncKey = 0x10A3DE0;  // remounted 2026-08-06 UL.OnFuncKey
constexpr uint32_t kRvaFuncKeyCtor = 0x1669410;  // remounted 2026-08-06 .ctor(FuncType,int)
// UIUtilDialogEx：SetKeyFocus(int) / OnClickBtOk
constexpr uint32_t kRvaUiDlgSelectMenu = 0x798770;  // remounted 2026-08-06 SetKeyFocus
constexpr uint32_t kRvaUiDlgOnClickBtOk = 0x7A1190;  // remounted 2026-08-06 OnClickBtOk
// UIShopDialog 产品买卖入口 — RVA 未漂；hash remount 2026-08-06
constexpr uint32_t kRvaSendSellRequestPacket = 0x55B010;
constexpr uint32_t kRvaSendBuyRequestPacket = 0x55A4C0;
constexpr uint32_t kRvaSendRechargeRequestPacket = 0x55B610;
constexpr uint32_t kRvaCmpSellItem = 0x55D000;
constexpr uint32_t kRvaShopSetRet = 0x548d70;
// UIDialog.Close — 基类虚函数；关 UI 实例
constexpr uint32_t kRvaUiDialogClose = 0x14BABD0;  // remounted 2026-08-20
// 方法哈希（dump 可读名缺失时的防漂；void(int) 在 UIShopDialog 上不唯一）
constexpr char kHashSendSell[] =
    "b14a3d1474afe56a0c6c11dd7a59195660a088a4513a1775fe230f97e695ff8";
constexpr char kHashSendBuy[] =
    "b0981e8fd5d4a33c3a43f4c3eea7d853be85eb17ceff2a6f9ea3572ee16a051";
constexpr char kHashSendRecharge[] =
    "a6c7903f9dc346c46a228c6a00325750c80e64c4b51e17768f5983bf68d7f29";
constexpr char kHashCmpSell[] =
    "c88771754e7c1552bf2d8ba9f10a1baa030d2ec6a0644184daef5e8be58748c";
constexpr char kHashSendPacket[] =
    "c9ab47fdd2dd9bce011a542e0877434b78da1951dcb24b26cb3335b0f0c3eec";
constexpr char kHashTalkToNpc[] =
    "bf9394a34beebd13f2c0af49a4b495878e794a61fd51e12b49f7a4612f9cdf1";
constexpr char kHashOnFuncKey[] =
    "e5caecb8cad10608bc617ed26e2566e53ff3305965a740cb2c0d5b4fa4d0f4e";
constexpr char kHashUiTabOnClick[] =
    "f19a9ddf3688c27e965a97bd3ed0722e0e48f2eec930499480c7d96c8fec8e4";
constexpr char kHashSetKeyFocus[] =
    "da96d1d9d7b85bf4c665462e633bd70b5862869c92b3249044a03b890ba60c8";
// OutPacket Create/Encode* — SEND OutPacket 13775；Encode1 本 port 用 sbyte
constexpr char kHashOutCreate[] =
    "f81b173e66885e22b09f4da7f0274b780c9cdfb60f49a491094659800f7b345";
constexpr char kHashEncode1Sbyte[] =
    "ce43eaecf651f8c713186746350842e65883e465294f104cbfed8c3e86fd0aa";
constexpr char kHashEncode2Short[] =
    "aaffb976d45e8ac5017166c83de8dff5ebd9e0fbe1b8c505a3fb75f1163f118";
constexpr char kHashEncode4Int[] =
    "d0dca07d6b4573864a0066a44c4f534ba563ab5200ba99fa67648422b757ced";
constexpr char kOutPacketClass[] =
    "b74303ab73cbc42ed6d4c0098d32f315dfffe76c4a7d6379bbdf53a9c804842";
// Unity helpers（明文名稳定）— 走 ResolveUnityMi
constexpr uint32_t kRvaButtonPress = 0x4FF3110;  // remounted 2026-08-06 Button.Press
constexpr uint32_t kRvaGetGameObject = x::runtime::il2cpp::kRvaCompGetGo;
constexpr uint32_t kRvaGoSetActive = 0x4E97EE0;  // remounted 2026-08-06 GameObject.set_active
constexpr uint32_t kRvaGoGetActiveSelf = 0x4E98080;  // remounted 2026-08-06 get_activeSelf
constexpr uint32_t kRvaUiTabOnClickTab = 0xB2BDF0;  // remounted 2026-08-06 UITab.OnClickTab
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

// Session/NM 方法宿主（与 il2cpp_shape::kHashNetworkManager 同）
constexpr char kSessionClass[] =
    "af2ed9237ad8d68031b6be7d48bbb5d2a14b278b0e44d7c3faa532a8b771977";  // remounted 2026-08-06

// 背包 / Money：SSOT = x::ui::player（hash→field_get_offset）；禁止再钉 WM/CD/CS 偏移。
#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())
#define kOffArrLen (x::runtime::il2cpp_container::OffArrayMaxLength())
#define kOffArrData (x::runtime::il2cpp_container::OffArrayData())
constexpr size_t kFbNpcPoolList = 0x10;  // NpcPool._npcList
constexpr char kHashNpcPoolList[] =
    "e92d6d431089f279d72e8f28a6bd0d0294cce82de19cc7f2488c3c0700b068e";
size_t gOffNpcPoolList = kFbNpcPoolList;
#define kOffNpcPoolList (gOffNpcPoolList)

constexpr char kNpcClass[] =
    "f8f0ff180f23cde30d99b6c43b2701ad1d22112cb9e60650853d52c96f9aaf6";  // remounted 2026-08-06
constexpr char kNpcDataClass[] =
    "eab681c335f3e1374c25e1c2ee425e3e34e8e4019214694ff815e3ed6c056b0";
constexpr char kActorBaseClass[] =
    "dbfdaecfa6ec49a19a019f06093eac445b3ca6082f2b874b489646c98303d44";  // = teleport
constexpr char kPacketClass[] =
    "c848eff7a1bfc6c43a334eefc656232eb016c7475e61e2199a5072889e7e46a";  // Packet base 13773

constexpr char kHashActorPos[] =
    "cf84c347e6d42aeed6a7320ec46ce5e76cd339550ee0f05e83b2508b331eef8";
constexpr char kHashNpcObjectId[] =
    "<be5ba8d25582072793528679c1fdf05310bd0fc1814fd0c54fade9775da020f>k__BackingField";
constexpr char kHashNpcData[] =
    "a1d6bbf415fdca93c12d211214eb5be4461438f6e5fc0d551d79dafbe38be9b";
constexpr char kHashNpcDataId[] =
    "a785a6bd971f556dd93d38b116cbd1e6ed314f9354f10e4dbc7c5d3850316a0";
constexpr char kHashUiDlgType[] =
    "ba297c8ee593291b884cc464db3f6b339b43657d8db5cac11da65953731b5b7";
constexpr char kHashUiDlgMenuTexts[] =
    "<d9c6799116404671404a064a8bfd9c1381fe18f1cbf2a4f88278127bdbefc7d>k__BackingField";
// Packet offset（基类）；SEND OutPacket id@0x20（非 InPacket backing）
constexpr char kHashPacketOffset[] =
    "<ae74f2c03cb7e81c627f408ec3dfe8c489da3f305e391a23adfd23328c7c47c>k__BackingField";
constexpr char kHashOutPacketId[] =
    "b272464a1cc05b43e57ae055661805d7df3f3df5e81c85a338a0c24c8e63dd9";

constexpr size_t kFbActorPos = 0x64, kFbNpcObjectId = 0x78, kFbNpcData = 0x80;
constexpr size_t kFbNpcDataId = 0x10, kFbUiDlgType = 0xA0, kFbUiDlgMenuTexts = 0xE0;
constexpr size_t kFbOutPacketId = 0x20, kFbPacketOffset = 0x18;
size_t gOffActorPos = kFbActorPos, gOffNpcObjectId = kFbNpcObjectId, gOffNpcData = kFbNpcData;
size_t gOffNpcDataId = kFbNpcDataId, gOffUiDlgType = kFbUiDlgType;
size_t gOffUiDlgMenuTexts = kFbUiDlgMenuTexts, gOffOutPacketId = kFbOutPacketId;
size_t gOffPacketOffset = kFbPacketOffset;
#define kOffActorPos (gOffActorPos)
#define kOffNpcObjectId (gOffNpcObjectId)
#define kOffNpcData (gOffNpcData)
#define kOffNpcDataId (gOffNpcDataId)
#define kOffUiDlgType (gOffUiDlgType)
#define kOffUiDlgMenuTexts (gOffUiDlgMenuTexts)
#define kOffOutPacketId (gOffOutPacketId)
#define kOffPacketOffset (gOffPacketOffset)
constexpr size_t kOffCachedPtr = 0x10;
#define kOffNmSession (x::runtime::il2cpp_network::OffNmSession())
#define kOffNmSessionState (x::runtime::il2cpp_network::OffNmSessionState())
#define kOffNmOpcodeHashSet (x::runtime::il2cpp_network::OffNmOpcodeHashSet())
#define kOffSessionState (x::runtime::il2cpp_network::OffSessionState())

// —— UIShopDialog / Item / UITab 字段防漂移：hash→field_get_offset；下列仅 dump fallback ——
constexpr size_t kFbBuyItemList0 = 0x178;
constexpr size_t kFbBuyItemList1 = 0x180;
constexpr size_t kFbSellItemList = 0x198;
constexpr size_t kFbBuySelectedIndex = 0x1A8;
constexpr size_t kFbSellSelectedIndex = 0x1AC;
constexpr size_t kFbLastBuyIndex = 0x1B0;
constexpr size_t kFbHasShopRequestSent = 0x1B4;
constexpr size_t kFbLastSellIndex = 0x1B8;
constexpr size_t kFbShopUiTab0 = 0xC0;
constexpr size_t kFbShopUiTab1 = 0xC8;
constexpr size_t kFbShopButtonExit = 0xA8;
constexpr size_t kFbShopItemId = 0x10;
constexpr size_t kFbShopItemPos = 0x14;
constexpr size_t kFbShopItemPrice = 0x28;
constexpr size_t kFbShopItemUnitPrice = 0x30;
constexpr size_t kFbShopItemMaxSlot = 0x38;
constexpr size_t kFbShopItemStock = 0x3C;  // CmpSellItem 比数量；卖栏投影常为 0
constexpr size_t kFbShopItemQty = 0x40;
constexpr size_t kFbShopItemSlot = 0x48;  // ItemSlotBase*；Charge 真源数量在 Bundle.nNumber
constexpr size_t kFbUiTabCurrentIndex = 0x20;
constexpr size_t kFbUiTabItems = 0x28;

// UIShopDialog 私有字段哈希（dump.cs TDI 434 · remount 2026-08-06；偏移未漂）
constexpr char kHashFldBuyList0[] =
    "eb0910291bcf467002fb96fe9e322de7ede5ab1265c61231951f7ba38c4bd5b";  // _buyItemList
constexpr char kHashFldBuyList1[] =
    "bef4ce5a1c9d726eb07adbacf0791a28b4ebe60f4dfef00a77042b3bf0e0c81";  // _buyItemRecommendedList
constexpr char kHashFldSellList[] =
    "aed70ae7c722d33e96eb15caeb45b6aaf54f852c1e5a26d0ec0b0d56213b0fb";  // _sellItemList
constexpr char kHashFldBuySelected[] =
    "e06d29962566727589cec5ccd7867bd4d294931865c3eb6813854f6cf3e2936";  // _buySelectedIndex
constexpr char kHashFldSellSelected[] =
    "c7e213ac67e457ef50033b23977d31b4872b6b285db1329951040164eef8c15";  // _sellSelectedIndex
constexpr char kHashFldLastBuy[] =
    "cec337438617a8977e0d6f1dde4888512baa08399a07c31bc4f75df0e8e03f2";  // lastBuy
constexpr char kHashFldHasRequest[] =
    "f896abf31f614b30a644c84e876072bed46c9e20a3785b9263f1abad3fc15e5";  // _hasShopRequestSent
constexpr char kHashFldLastSell[] =
    "bae123cab057ec4ee70049a8ca8f21b8cd15fdb59e97ccf6169079af019b8af";  // _lastSellIndex
constexpr char kHashFldUiTab0[] =
    "db8f019130f1df7bbdfcdfdcca60a4b4a170aacb4f8a8485b0554149fb439fd";
constexpr char kHashFldUiTab1[] =
    "e4c485c8ab908281ab96a0a07af25f112bb0cb604811b1f819841498251ee34";
constexpr char kHashFldButtonExit[] =
    "d69b7543393cbe5394f522f3048346b495e6acc4b9b04fe8597ff4c777054b5";

// Item DTO（TDI 435）· remount 2026-08-06（明文名已哈希；偏移未漂）
constexpr char kFldItemId[] =
    "e5fd17ecb06a6d1cf100e82a0564d562134f9c1f7bf0661aaa27ba4306c1156";
constexpr char kFldItemPos[] =
    "d3feccfb9526e82ea6e4cfd720b2ce8687ccb03fbad4fbe09ad95080f6c7d03";
constexpr char kFldItemPrice[] =
    "d98e69d9ba09db47a1bdf4a5e4ccc0bc84800c525048bc34e0a7aa127075a1e";
constexpr char kFldItemUnitPrice[] =
    "eb684d9313baf57ed23a808c8d953cd98cdbd3aedb1985ee4c58e63e7000809";  // double@0x30
constexpr char kFldItemMaxSlot[] =
    "f787662111f8bb4742b9deedcc580c8d0f4b96cfc721b53ec67b2a845685e02";
constexpr char kFldItemQty[] =
    "dbb838aaaee660a0ba65ccbac54867cbaa6463b48589cb54a161afd44859a9a";
constexpr char kFldUiTabCurrent[] =
    "cf28eeb9bbc7d8a2e3eea7261afc3bbae8b1130feea848ba4b2e5061773f23c";
constexpr char kFldUiTabItems[] =
    "b046a570e9880f6f42d854c7b1482a0a268ee750c8e307eda7f4b8f7ea49838";
// ShopItem klass hash（internal nested TDI 435；FindShopItemKlass 鉴别用）
constexpr char kShopItemClass[] =
    "f0688235bcabb4c81d6532e32ffd75212949352d61bc5e7f2e136c388071ddc";

struct ShopFieldOff {
    size_t buyList0 = kFbBuyItemList0;
    size_t buyList1 = kFbBuyItemList1;
    size_t sellList = kFbSellItemList;
    size_t buySelected = kFbBuySelectedIndex;
    size_t sellSelected = kFbSellSelectedIndex;
    size_t lastBuy = kFbLastBuyIndex;
    size_t hasRequest = kFbHasShopRequestSent;
    size_t lastSell = kFbLastSellIndex;
    size_t uiTab0 = kFbShopUiTab0;
    size_t uiTab1 = kFbShopUiTab1;
    size_t buttonExit = kFbShopButtonExit;
    size_t itemId = kFbShopItemId;
    size_t itemPos = kFbShopItemPos;
    size_t itemPrice = kFbShopItemPrice;
    size_t itemUnitPrice = kFbShopItemUnitPrice;
    size_t itemMaxSlot = kFbShopItemMaxSlot;
    size_t itemStock = kFbShopItemStock;
    size_t itemQty = kFbShopItemQty;
    size_t itemSlot = kFbShopItemSlot;
    size_t tabCurrent = kFbUiTabCurrentIndex;
    size_t tabItems = kFbUiTabItems;
    bool tried = false;
    int hits = 0;
    const char* path = "fallback";  // meta | meta-partial | fallback
};
ShopFieldOff gOff{};

#define kOffBuyItemList0 (gOff.buyList0)
#define kOffBuyItemList1 (gOff.buyList1)
#define kOffSellItemList (gOff.sellList)
#define kOffBuySelectedIndex (gOff.buySelected)
#define kOffSellSelectedIndex (gOff.sellSelected)
#define kOffLastBuyIndex (gOff.lastBuy)
#define kOffHasShopRequestSent (gOff.hasRequest)
#define kOffLastSellIndex (gOff.lastSell)
#define kOffShopUiTab0 (gOff.uiTab0)
#define kOffShopUiTab1 (gOff.uiTab1)
#define kOffShopButtonExit (gOff.buttonExit)
#define kOffShopItemId (gOff.itemId)
#define kOffShopItemPos (gOff.itemPos)
#define kOffShopItemPrice (gOff.itemPrice)
#define kOffShopItemUnitPrice (gOff.itemUnitPrice)
#define kOffShopItemMaxSlot (gOff.itemMaxSlot)
#define kOffShopItemStock (gOff.itemStock)
#define kOffShopItemQty (gOff.itemQty)
#define kOffShopItemSlot (gOff.itemSlot)
#define kOffUiTabCurrentIndex (gOff.tabCurrent)
#define kOffUiTabItems (gOff.tabItems)

constexpr int kShurikenIdMin = 2070000;
constexpr int kShurikenIdMax = 2079999;
// 卖栏 Item.MaxSlotCount 常为 0。满格真源：ItemBundle.nMaxPerSlot / Info.slotMax（按 itemId）。
// 仅 IDM 全失败时才退回 500（BIN：部分飞镖满格=500；勿再用统一 800）。
constexpr int kShurikenDefaultMaxSlot = 500;
constexpr int kSessionStateConnected = 3;

// ItemDataManager : Singleton<>（TDI 2027）— 与 titlebar 同源 hash
constexpr char kItemDataManagerClass[] =
    "aac3cf416aa08bf0ee465a5734c59a41f3accd43ae7174fc4512700fd2b51e4";
constexpr char kItemDataClass[] =
    "cf36147aca190f8fa6f5c0fbfdc3d5719759bc5178de4fbce837bd625355c92";
constexpr char kItemBundleClass[] =
    "d08982d4d0f19de91e0135408c1ac7c707b54487cf3fd0490dacb77b74754d2";
constexpr char kItemInfoClass[] =
    "f00b14f120438650475eb0c92e6a8a7889cff06410de984532203b153a1c888";
constexpr char kHashIdmDataTable[] =
    "e2b9db4f333894c0e1428d16acd71cf6461484cb9485e5417d2852961651385";
constexpr char kHashIdmBundleMap[] =
    "bb263303decb7f73baf70705dc76842f8599d7a6d7835df295a72fe8825b651";
constexpr char kHashItemDataInfo[] =
    "f04e114fd91478048f2179f6422e4d7ff563c87a277f1c45c6c09b2f7a69bd0";
// 运行时字段名已哈希；明文 nMaxPerSlot/slotMax 会 field_get_offset miss
constexpr char kHashBundleMaxPerSlot[] =
    "a14fc5f14eac9b22405de7bbdffede524e3874a571911c557e180452a37c36a";
constexpr char kHashInfoSlotMax[] =
    "f928aca13ed906b9c0da7ae8efa10d11477cd97737c3d56e2f952056278862d";
constexpr size_t kFbIdmDataTable = 0x18;
constexpr size_t kFbIdmBundleMap = 0x38;       // Dictionary<int, ItemBundle>
constexpr size_t kFbItemDataInfo = 0x18;
constexpr size_t kFbBundleMaxPerSlot = 0x4C;  // short nMaxPerSlot
constexpr size_t kFbInfoSlotMax = 0x58;       // int slotMax
size_t gOffIdmDataTable = kFbIdmDataTable;
size_t gOffIdmBundleMap = kFbIdmBundleMap;
size_t gOffItemDataInfo = kFbItemDataInfo;
size_t gOffBundleMaxPerSlot = kFbBundleMaxPerSlot;
size_t gOffInfoSlotMax = kFbInfoSlotMax;
#define kOffIdmDataTable (gOffIdmDataTable)
#define kOffIdmBundleMap (gOffIdmBundleMap)
#define kOffItemDataInfo (gOffItemDataInfo)
#define kOffBundleMaxPerSlot (gOffBundleMaxPerSlot)
#define kOffInfoSlotMax (gOffInfoSlotMax)
void* gItemDataManagerKlass = nullptr;
void* gItemDataManager = nullptr;
DWORD gLastIdmRebind = 0;
bool gIdmFieldTried = false;
std::unordered_map<int, int> gItemMaxSlotCache;

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
using FnSendRechargePacket = void (*)(void* self, const void* method);
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
MethodInfoHead* gMiSendRechargePacket = nullptr;
MethodInfoHead* gMiCmpSellItem = nullptr;
MethodInfoHead* gMiShopSetRet = nullptr;
MethodInfoHead* gMiUiDialogClose = nullptr;
MethodInfoHead* gMiUiDlgOnClickOk = nullptr;
MethodInfoHead* gMiUiDlgSelectMenu = nullptr;
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

double ReadF64(void* obj, size_t off) {
    if (!obj) return 0.0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
}

bool PlausibleInstanceOff(size_t off) {
    return off >= 0x10 && off < 0x1000;
}

bool FieldOffOrFb(void* klass, const char* fieldName, size_t fb, size_t* out) {
    *out = fb;
    if (!klass || !fieldName) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(klass, fieldName);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!field) return false;
    size_t off = 0;
    __try {
        off = e.fieldGetOffset(field);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!PlausibleInstanceOff(off)) return false;
    *out = off;
    return true;
}

using FnClassGetNestedTypes = void* (*)(void* klass, void** iter);

void* FindShopItemKlass(void* shopKlass) {
    if (!shopKlass) return nullptr;
    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    if (ga) {
        auto nested = reinterpret_cast<FnClassGetNestedTypes>(
            GetProcAddress(ga, "il2cpp_class_get_nested_types"));
        const auto& e = x::runtime::il2cpp::Get();
        if (nested && e.classGetFieldFromName) {
            void* iter = nullptr;
            __try {
                for (;;) {
                    void* nk = nested(shopKlass, &iter);
                    if (!nk) break;
                    void* fUnit = e.classGetFieldFromName(nk, kFldItemUnitPrice);
                    void* fId = e.classGetFieldFromName(nk, kFldItemId);
                    if (fUnit && fId) return nk;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    // 明文 Item 已死；按 ShopItem hash / UintPrice 鉴别字段回退
    void* cand = x::runtime::il2cpp::FindClass("", kShopItemClass);
    if (cand) {
        const auto& e = x::runtime::il2cpp::Get();
        if (e.classGetFieldFromName) {
            void* fUnit = nullptr;
            __try {
                fUnit = e.classGetFieldFromName(cand, kFldItemUnitPrice);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                fUnit = nullptr;
            }
            if (fUnit) return cand;
        }
    }
    return cand;
}

void EnsureShopFieldOffsets() {
    if (gOff.tried) return;
    gOff.tried = true;
    if (!x::runtime::il2cpp::Ensure()) {
        x::runtime::LogW("Shop", "field offsets: bind miss — dump fallbacks");
        return;
    }
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) {
        x::runtime::LogW("Shop", "field offsets: exports miss — dump fallbacks");
        return;
    }

    void* shopKlass = gShopDlgKlass;
    if (!shopKlass) {
        shopKlass =
            x::runtime::il2cpp_prefab::FindClassCached(kUiShopDialogClass, kPrefabShopDialog).klass;
        if (shopKlass) gShopDlgKlass = shopKlass;
    }
    void* tabKlass = x::runtime::il2cpp::FindClass("", "UITab");
    if (!tabKlass)
        tabKlass = x::runtime::il2cpp::FindClass(
            "", "c9c8c8fbda43943bb43465d24b1c29f93c6ad4f794241f571b51ca3569c9491");
    void* itemKlass = FindShopItemKlass(shopKlass);

    int hits = 0;
    auto hit = [&](bool ok) {
        if (ok) ++hits;
    };

    hit(FieldOffOrFb(shopKlass, kHashFldBuyList0, kFbBuyItemList0, &gOff.buyList0));
    hit(FieldOffOrFb(shopKlass, kHashFldBuyList1, kFbBuyItemList1, &gOff.buyList1));
    hit(FieldOffOrFb(shopKlass, kHashFldSellList, kFbSellItemList, &gOff.sellList));
    hit(FieldOffOrFb(shopKlass, kHashFldBuySelected, kFbBuySelectedIndex, &gOff.buySelected));
    hit(FieldOffOrFb(shopKlass, kHashFldSellSelected, kFbSellSelectedIndex, &gOff.sellSelected));
    hit(FieldOffOrFb(shopKlass, kHashFldLastBuy, kFbLastBuyIndex, &gOff.lastBuy));
    hit(FieldOffOrFb(shopKlass, kHashFldHasRequest, kFbHasShopRequestSent, &gOff.hasRequest));
    hit(FieldOffOrFb(shopKlass, kHashFldLastSell, kFbLastSellIndex, &gOff.lastSell));
    hit(FieldOffOrFb(shopKlass, kHashFldUiTab0, kFbShopUiTab0, &gOff.uiTab0));
    hit(FieldOffOrFb(shopKlass, kHashFldUiTab1, kFbShopUiTab1, &gOff.uiTab1));
    hit(FieldOffOrFb(shopKlass, kHashFldButtonExit, kFbShopButtonExit, &gOff.buttonExit));

    hit(FieldOffOrFb(itemKlass, kFldItemId, kFbShopItemId, &gOff.itemId));
    hit(FieldOffOrFb(itemKlass, kFldItemPos, kFbShopItemPos, &gOff.itemPos));
    hit(FieldOffOrFb(itemKlass, kFldItemPrice, kFbShopItemPrice, &gOff.itemPrice));
    hit(FieldOffOrFb(itemKlass, kFldItemUnitPrice, kFbShopItemUnitPrice, &gOff.itemUnitPrice));
    hit(FieldOffOrFb(itemKlass, kFldItemMaxSlot, kFbShopItemMaxSlot, &gOff.itemMaxSlot));
    hit(FieldOffOrFb(itemKlass, kFldItemQty, kFbShopItemQty, &gOff.itemQty));
    // Stock@MaxSlot+4：dump 明文可解析；哈希化时用 MaxSlot/Quantity 夹心推导（0x38/0x3C/0x40）
    {
        bool stockOk = FieldOffOrFb(itemKlass, "Stock", kFbShopItemStock, &gOff.itemStock);
        if (!stockOk && gOff.itemQty == gOff.itemMaxSlot + 8) {
            gOff.itemStock = gOff.itemMaxSlot + 4;
            stockOk = true;
        }
        hit(stockOk);
    }
    hit(FieldOffOrFb(itemKlass, "ItemSlot", kFbShopItemSlot, &gOff.itemSlot));

    hit(FieldOffOrFb(tabKlass, kFldUiTabCurrent, kFbUiTabCurrentIndex, &gOff.tabCurrent));
    hit(FieldOffOrFb(tabKlass, kFldUiTabItems, kFbUiTabItems, &gOff.tabItems));

    void* npcPoolKlass =
        gNpcPoolKlass ? gNpcPoolKlass : x::runtime::il2cpp::FindClass("", kNpcPoolClass);
    if (npcPoolKlass) gNpcPoolKlass = npcPoolKlass;
    hit(FieldOffOrFb(npcPoolKlass, kHashNpcPoolList, kFbNpcPoolList, &gOffNpcPoolList));

    void* npcKlass = x::runtime::il2cpp::FindClass("", kNpcClass);
    void* npcDataKlass = x::runtime::il2cpp::FindClass("", kNpcDataClass);
    void* actorKlass = x::runtime::il2cpp::FindClass("", kActorBaseClass);
    if (!actorKlass) actorKlass = npcKlass;
    void* uiDlgKlass = gUiDlgKlass
                           ? gUiDlgKlass
                           : x::runtime::il2cpp::FindClass("", kUiUtilDialogExClass);
    void* pktKlass = gOutPacketKlass
                         ? gOutPacketKlass
                         : x::runtime::il2cpp::FindClass("", kOutPacketClass);
    if (!pktKlass) pktKlass = x::runtime::il2cpp::FindClass("", kPacketClass);
    hit(FieldOffOrFb(actorKlass, kHashActorPos, kFbActorPos, &gOffActorPos));
    hit(FieldOffOrFb(npcKlass, kHashNpcObjectId, kFbNpcObjectId, &gOffNpcObjectId));
    hit(FieldOffOrFb(npcKlass, kHashNpcData, kFbNpcData, &gOffNpcData));
    hit(FieldOffOrFb(npcDataKlass, kHashNpcDataId, kFbNpcDataId, &gOffNpcDataId));
    hit(FieldOffOrFb(uiDlgKlass, kHashUiDlgType, kFbUiDlgType, &gOffUiDlgType));
    hit(FieldOffOrFb(uiDlgKlass, kHashUiDlgMenuTexts, kFbUiDlgMenuTexts, &gOffUiDlgMenuTexts));
    hit(FieldOffOrFb(pktKlass, kHashOutPacketId, kFbOutPacketId, &gOffOutPacketId));
    // PacketOffset 在基类 Packet 上
    void* pktBase = x::runtime::il2cpp::FindClass("", kPacketClass);
    if (!pktBase) pktBase = pktKlass;
    hit(FieldOffOrFb(pktBase, kHashPacketOffset, kFbPacketOffset, &gOffPacketOffset));

    constexpr int kExpect = 30;
    gOff.hits = hits;
    gOff.path = hits == kExpect ? "meta" : (hits ? "meta-partial" : "fallback");
    x::runtime::LogI(
        "Shop",
        "field offsets path=%s hits=%d/%d sellList=0x%zx npcData=0x%zx dlgType=0x%zx "
        "pktId=0x%zx itemKlass=%p",
        gOff.path, hits, kExpect, gOff.sellList, gOffNpcData, gOffUiDlgType, gOffOutPacketId,
        itemKlass);
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
    const int16_t n = ReadI16(slot, x::ui::player::OffSlotBundleNumber());
    return n > 0 ? static_cast<int>(n) : 1;
}

// Charge 用：允许 0；读失败返回 -1（勿用 ItemQty：n<=0 会落成 1）
int BundleNumberRaw(void* slot) {
    if (!LooksLikeHeapPtr(slot)) return -1;
    const int16_t n = ReadI16(slot, x::ui::player::OffSlotBundleNumber());
    if (n < 0) return -1;
    return static_cast<int>(n);
}

void* FindClass(const char* name);
void* TryLazyValue(void* lazy);

#define kOffDictBuckets (x::runtime::il2cpp_container::OffDictBuckets())
#define kOffDictEntries (x::runtime::il2cpp_container::OffDictEntries())
#define kOffDictCount (x::runtime::il2cpp_container::OffDictCount())
#define kOffDictFreeCount (x::runtime::il2cpp_container::OffDictFreeCount())
#define kOffEntryHash (x::runtime::il2cpp_container::OffDictEntryHash())
#define kOffEntryNext (x::runtime::il2cpp_container::OffDictEntryNext())
#define kOffEntryKey (x::runtime::il2cpp_container::OffDictEntryKey())
#define kOffEntryValue (x::runtime::il2cpp_container::OffDictEntryValuePtr())
#define kEntrySize (x::runtime::il2cpp_container::DictEntryStrideIntPtr())

int ArrayI32At(void* array, uintptr_t index) {
    if (!LooksLikeHeapPtr(array)) return -1;
    __try {
        if (index >= ArrayLen(array)) return -1;
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(array) + kOffArrData +
                                           index * sizeof(int32_t));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void* DictGetIntPtr(void* dictionary, int key) {
    if (!LooksLikeHeapPtr(dictionary)) return nullptr;
    void* entries = ReadPtr(dictionary, kOffDictEntries);
    const int count = ReadI32(dictionary, kOffDictCount);
    const int freeCount = ReadI32(dictionary, kOffDictFreeCount);
    const uintptr_t entryCount = ArrayLen(entries);
    if (!LooksLikeHeapPtr(entries) || count <= 0 || count > 200000 || entryCount == 0 ||
        entryCount > 400000) {
        return nullptr;
    }
    const int hashCode = key & 0x7fffffff;
    void* buckets = ReadPtr(dictionary, kOffDictBuckets);
    if (LooksLikeHeapPtr(buckets)) {
        const uintptr_t bucketCount = ArrayLen(buckets);
        if (bucketCount > 0 && bucketCount <= 400000) {
            int index = ArrayI32At(buckets, static_cast<uintptr_t>(hashCode) % bucketCount);
            const int limit = count > freeCount ? count - freeCount + 8 : count + 8;
            for (int guard = 0; index >= 0 && guard < limit &&
                                static_cast<uintptr_t>(index) < entryCount; ++guard) {
                uint8_t* entry =
                    x::runtime::il2cpp_container::DictEntryAt(entries, index, kEntrySize);
                if (!entry) break;
                if (ReadI32(entry, kOffEntryHash) == hashCode &&
                    ReadI32(entry, kOffEntryKey) == key) {
                    void* value = ReadPtr(entry, kOffEntryValue);
                    return LooksLikeHeapPtr(value) ? value : nullptr;
                }
                index = ReadI32(entry, kOffEntryNext);
            }
            return nullptr;
        }
    }
    for (uintptr_t index = 0; index < entryCount; ++index) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(
            entries, static_cast<int>(index), kEntrySize);
        if (!entry) continue;
        if (ReadI32(entry, kOffEntryHash) < 0 || ReadI32(entry, kOffEntryKey) != key) continue;
        void* value = ReadPtr(entry, kOffEntryValue);
        return LooksLikeHeapPtr(value) ? value : nullptr;
    }
    return nullptr;
}

void EnsureIdmFieldOffsets() {
    if (gIdmFieldTried) return;
    gIdmFieldTried = true;
    x::runtime::il2cpp_container::Ensure();
    void* idmKlass = FindClass(kItemDataManagerClass);
    void* dataKlass = FindClass(kItemDataClass);
    void* bundleKlass = FindClass(kItemBundleClass);
    void* infoKlass = FindClass(kItemInfoClass);
    gItemDataManagerKlass = idmKlass;
    int hits = 0;
    if (FieldOffOrFb(idmKlass, kHashIdmDataTable, kFbIdmDataTable, &gOffIdmDataTable)) ++hits;
    if (FieldOffOrFb(idmKlass, kHashIdmBundleMap, kFbIdmBundleMap, &gOffIdmBundleMap)) ++hits;
    if (FieldOffOrFb(dataKlass, kHashItemDataInfo, kFbItemDataInfo, &gOffItemDataInfo)) ++hits;
    if (FieldOffOrFb(bundleKlass, kHashBundleMaxPerSlot, kFbBundleMaxPerSlot,
                     &gOffBundleMaxPerSlot))
        ++hits;
    if (FieldOffOrFb(infoKlass, kHashInfoSlotMax, kFbInfoSlotMax, &gOffInfoSlotMax)) ++hits;
    x::runtime::LogI("Shop",
                     "IDM slotMax fields hits=%d/%d bundleMap=0x%zx nMaxPerSlot=0x%zx "
                     "info=0x%zx slotMax=0x%zx",
                     hits, 5, gOffIdmBundleMap, gOffBundleMaxPerSlot, gOffItemDataInfo,
                     gOffInfoSlotMax);
}

bool LooksLikeItemDataManager(void* cand) {
    if (!LooksLikeHeapPtr(cand)) return false;
    if (gItemDataManagerKlass) {
        void* k = ReadPtr(cand, 0);
        if (k != gItemDataManagerKlass) return false;
    }
    void* bundleMap = ReadPtr(cand, kOffIdmBundleMap);
    if (!LooksLikeHeapPtr(bundleMap)) return false;
    const int count = ReadI32(bundleMap, kOffDictCount);
    return count >= 0 && count < 500000;
}

void* ResolveItemDataManager() {
    EnsureIdmFieldOffsets();
    constexpr DWORD kRebindMs = 3000;
    const DWORD now = GetTickCount();
    if (LooksLikeItemDataManager(gItemDataManager)) return gItemDataManager;
    if (gLastIdmRebind && now - gLastIdmRebind < kRebindMs && !gItemDataManager) return nullptr;
    gLastIdmRebind = now;
    gItemDataManager = nullptr;
    if (!gItemDataManagerKlass) gItemDataManagerKlass = FindClass(kItemDataManagerClass);
    if (!gItemDataManagerKlass) return nullptr;

    const auto& e = x::runtime::il2cpp::Get();
    // Charge 只在 MainPump 调；允许 class_init。勿在 worker 复用本函数碰托管。
    if (e.runtimeClassInit) x::runtime::il2cpp::RuntimeClassInit(gItemDataManagerKlass);

    void* staticsKlass = gItemDataManagerKlass;
    void* parent = nullptr;
    if (e.classParent) {
        __try {
            parent = e.classParent(gItemDataManagerKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
        if (parent) {
            if (e.runtimeClassInit) x::runtime::il2cpp::RuntimeClassInit(parent);
            staticsKlass = parent;
        }
    }

    auto staticsOf = [&](void* k) -> void* {
        if (!k || !e.classStaticData) return nullptr;
        __try {
            void* sd = e.classStaticData(k);
            return LooksLikeHeapPtr(sd) ? sd : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    };

    void* best = nullptr;
    auto scan = [&](void* sd) {
        if (!sd || best) return;
        for (size_t s = 0; s <= 0x40; s += sizeof(void*)) {
            void* lazy = ReadPtr(sd, s);
            void* cand = TryLazyValue(lazy);
            if (!cand) cand = lazy;
            if (LooksLikeItemDataManager(cand)) {
                best = cand;
                return;
            }
        }
    };
    scan(staticsOf(staticsKlass));
    if (!best) scan(staticsOf(gItemDataManagerKlass));

    if (best) {
        gItemDataManager = best;
        x::runtime::LogI("Shop", "ItemDataManager ACCEPT idm=%p", gItemDataManager);
    } else {
        x::runtime::LogWThrottled(41, 15000, "Shop", "ItemDataManager resolve miss klass=%p",
                                  gItemDataManagerKlass);
    }
    return gItemDataManager;
}

// 返回 >0 的堆叠上限；失败 0。优先 ItemBundle.nMaxPerSlot，其次 Info.slotMax。
int LookupItemMaxPerSlot(int itemId) {
    if (itemId <= 0) return 0;
    const auto it = gItemMaxSlotCache.find(itemId);
    if (it != gItemMaxSlotCache.end()) return it->second;

    int maxSlot = 0;
    void* idm = ResolveItemDataManager();
    if (LooksLikeHeapPtr(idm)) {
        if (void* bundle = DictGetIntPtr(ReadPtr(idm, kOffIdmBundleMap), itemId)) {
            const int16_t n = ReadI16(bundle, kOffBundleMaxPerSlot);
            if (n > 0) maxSlot = static_cast<int>(n);
        }
        if (maxSlot <= 0) {
            if (void* data = DictGetIntPtr(ReadPtr(idm, kOffIdmDataTable), itemId)) {
                if (void* info = ReadPtr(data, kOffItemDataInfo); LooksLikeHeapPtr(info)) {
                    const int sm = ReadI32(info, kOffInfoSlotMax);
                    if (sm > 0) maxSlot = sm;
                }
            }
        }
    }
    gItemMaxSlotCache[itemId] = maxSlot;
    return maxSlot;
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
        x::runtime::il2cpp::RuntimeClassInit(k);
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

// hash → plain → RVA/kind（FindMethodResolved SSOT）。
MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plainName = nullptr, const char* hashName = nullptr,
                          x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plainName, hashName);
    if (outPath) *outPath = mr.path;
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
}

bool ResolveApi();

// Unity 无游戏哈希：FindMethodResolved = 明文 → RVA/kind（SetActive 靠 arity+RVA 避开 set_active）。
MethodInfoHead* ResolveUnityMi(void* klass, uint32_t rva, const char* plain,
                               const x::runtime::il2cpp_method::MethodShape& shape,
                               x::runtime::il2cpp_method::ResolvePath* outPath = nullptr) {
    if (outPath) *outPath = x::runtime::il2cpp_method::ResolvePath::Miss;
    if (!klass) return nullptr;
    const auto mr =
        x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, nullptr);
    if (outPath) *outPath = mr.path;
    if (mr.method && mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
        x::runtime::LogI("Shop", "ResolveUnityMi kind hit rva=0x%X plain=%s", rva,
                         plain ? plain : "-");
    }
    return mr.method ? reinterpret_cast<MethodInfoHead*>(mr.method) : nullptr;
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
        gMiButtonPress = ResolveUnityMi(btnKlass, kRvaButtonPress, "Press", kPress);
    }
    if (compKlass && !gMiGetGameObject) {
        constexpr MethodShape kGo{0, TypeKind::Ptr, true, true, {}};
        gMiGetGameObject =
            ResolveUnityMi(compKlass, kRvaGetGameObject, "get_gameObject", kGo);
    }
    if (goKlass) {
        // SetActive 与 set_active 同 void(bool)，unique kind 不可靠 → RVA+明文
        if (!gMiGoSetActive) {
            constexpr MethodShape kSet{1, TypeKind::Void, false, true, {TypeKind::Bool}};
            gMiGoSetActive =
                ResolveUnityMi(goKlass, kRvaGoSetActive, "SetActive", kSet);
        }
        if (!gMiGoGetActiveSelf) {
            constexpr MethodShape kAct{0, TypeKind::Bool, true, true, {}};
            gMiGoGetActiveSelf =
                ResolveUnityMi(goKlass, kRvaGoGetActiveSelf, "get_activeSelf", kAct);
        }
    }
    static bool sUnityHitsLogged = false;
    if (!sUnityHitsLogged && (gMiButtonPress || gMiGetGameObject || gMiGoSetActive || gMiGoGetActiveSelf)) {
        sUnityHitsLogged = true;
        const int hits = (gMiButtonPress ? 1 : 0) + (gMiGetGameObject ? 1 : 0) +
                         (gMiGoSetActive ? 1 : 0) + (gMiGoGetActiveSelf ? 1 : 0);
        x::runtime::LogI("Shop", "unity methods path=%s hits=%d/4",
                         hits == 4 ? "plain" : (hits ? "meta-partial" : "fallback"), hits);
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
        (gMiSendBuyPacket || (gGA && AtRva<void*>(kRvaSendBuyRequestPacket)))) {
        EnsureShopFieldOffsets();
        return true;
    }
    gLastRebindMs = now;
    if (!ResolveApi()) return false;
    BindUnityHelpers();

    if (!gShopDlgKlass)
        gShopDlgKlass =
            x::runtime::il2cpp_prefab::FindClassCached(kUiShopDialogClass, kPrefabShopDialog).klass;
    if (!gShopDlgType && gShopDlgKlass) gShopDlgType = ClassTypeObject(gShopDlgKlass);
    EnsureShopFieldOffsets();

    if (!gFacadeKlass) gFacadeKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerFacadeKlass();
    if (!gSessionKlass) gSessionKlass = x::runtime::il2cpp_shape::ResolveNetworkManagerKlass();
    gNmKlass = gSessionKlass;  // Send MethodInfo 宿主
    if (!gFacadeType && gFacadeKlass) gFacadeType = ClassTypeObject(gFacadeKlass);
    if (!gOutPacketKlass) {
        gOutPacketKlass = FindClass(kOutPacketClass);
        if (!gOutPacketKlass) gOutPacketKlass = FindClass("OutPacket");
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
                                reinterpret_cast<uint8_t*>(arr) + kOffArrLen))
                          : 0;
        for (int i = 0; i < n && i < 8; ++i) {
            void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
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
        // static OutPacket Create(PacketType/int)
        constexpr MethodShape kCreate{1, TypeKind::Ptr, true, false, {TypeKind::Any}};
        if (!gMiOutCreate)
            gMiOutCreate = ResolveMi(gOutPacketKlass, kRvaOutPacketCreate, kCreate, "Create",
                                     kHashOutCreate);
        // Encode*：hash 钉死重载；RVA 仅 fallback
        constexpr MethodShape kEnc{1, TypeKind::Void, true, false, {TypeKind::Any}};
        if (!gMiEncode1)
            gMiEncode1 = ResolveMi(gOutPacketKlass, kRvaOutPacketEncode1Byte, kEnc, "Encode1",
                                   kHashEncode1Sbyte);
        if (!gMiEncode2)
            gMiEncode2 = ResolveMi(gOutPacketKlass, kRvaOutPacketEncode2Short, kEnc, "Encode2",
                                   kHashEncode2Short);
        if (!gMiEncode4) {
            constexpr MethodShape kEnc4{1, TypeKind::Void, true, false, {TypeKind::I32}};
            gMiEncode4 = ResolveMi(gOutPacketKlass, kRvaOutPacketEncode4Int, kEnc4, "Encode4",
                                   kHashEncode4Int);
        }
    }
    using x::runtime::il2cpp_method::ResolvePath;
    int methodHashHits = 0;
    auto noteHash = [&](ResolvePath path) {
        if (path == ResolvePath::Hash) ++methodHashHits;
    };
    ResolvePath pSend = ResolvePath::Miss, pSell = ResolvePath::Miss, pBuy = ResolvePath::Miss,
                pCharge = ResolvePath::Miss, pCmp = ResolvePath::Miss, pMenu = ResolvePath::Miss;

    if (gNmKlass && !gMiSend) {
        // bool(OutPacket) — paramKlass 钉死，Session 上多个 bool(ptr) 可唯一。
        MethodShape kSend{};
        kSend.arity = 1;
        kSend.ret = TypeKind::Bool;
        kSend.unique = true;
        kSend.walkParents = true;
        kSend.param[0] = TypeKind::Ptr;
        if (gOutPacketKlass) kSend.paramKlass[0] = gOutPacketKlass;
        gMiSend = ResolveMi(gNmKlass, kRvaNmSend, kSend, "SendPacket", kHashSendPacket, &pSend);
        if (!gMiSend)
            gMiSend = ResolveMi(gNmKlass, kRvaNmSend, kSend, "Send", kHashSendPacket, &pSend);
        noteHash(pSend);
    }
    if (gShopDlgKlass && !gMiSendSellPacket) {
        constexpr MethodShape kSell{1, TypeKind::Void, true, false, {TypeKind::I32}};
        gMiSendSellPacket = ResolveMi(gShopDlgKlass, kRvaSendSellRequestPacket, kSell,
                                      "SendSellRequestPacket", kHashSendSell, &pSell);
        noteHash(pSell);
    }
    if (gShopDlgKlass && !gMiSendBuyPacket) {
        constexpr MethodShape kBuy{1, TypeKind::Void, true, false, {TypeKind::I32}};
        gMiSendBuyPacket = ResolveMi(gShopDlgKlass, kRvaSendBuyRequestPacket, kBuy,
                                     "SendBuyRequestPacket", kHashSendBuy, &pBuy);
        noteHash(pBuy);
    }
    if (gShopDlgKlass && !gMiSendRechargePacket) {
        constexpr MethodShape kCharge{0, TypeKind::Void, true, false, {}};
        gMiSendRechargePacket =
            ResolveMi(gShopDlgKlass, kRvaSendRechargeRequestPacket, kCharge,
                      "SendRechargeRequestPacket", kHashSendRecharge, &pCharge);
        noteHash(pCharge);
    }
    if (gShopDlgKlass && !gMiCmpSellItem) {
        constexpr MethodShape kCmp{0, TypeKind::I32, true, false, {}};
        gMiCmpSellItem =
            ResolveMi(gShopDlgKlass, kRvaCmpSellItem, kCmp, "CmpSellItem", kHashCmpSell, &pCmp);
        noteHash(pCmp);
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
    if (!gUiDlgKlass)
        gUiDlgKlass =
            x::runtime::il2cpp_prefab::FindClassCached(kUiUtilDialogExClass, kPrefabUtilDialogEx)
                .klass;
    if (gUiDlgKlass && !gMiUiDlgOnClickOk) {
        constexpr MethodShape kOk{0, TypeKind::Void, true, false, {}};
        gMiUiDlgOnClickOk =
            ResolveMi(gUiDlgKlass, kRvaUiDlgOnClickBtOk, kOk, "OnClickBtOk", nullptr);
    }
    if (gUiDlgKlass && !gMiUiDlgSelectMenu) {
        // void(int) 不唯一 → 哈希主（dump 无 SetKeyFocus 明文）
        constexpr MethodShape kMenu{1, TypeKind::Void, false, false, {TypeKind::I32}};
        gMiUiDlgSelectMenu =
            ResolveMi(gUiDlgKlass, kRvaUiDlgSelectMenu, kMenu, "SetKeyFocus", kHashSetKeyFocus,
                      &pMenu);
        noteHash(pMenu);
    }
    static bool sMethodHitsLogged = false;
    if (!sMethodHitsLogged) {
        sMethodHitsLogged = true;
        x::runtime::LogI("Shop", "methods path=%s hits=%d/6",
                         methodHashHits == 6 ? "meta"
                                             : (methodHashHits ? "meta-partial" : "fallback"),
                         methodHashHits);
    }
    // 买/卖均走 UIShopDialog.*RequestPacket；手组包已证实会本地踢线。
    return gShopDlgKlass && (gMiSendSellPacket || AtRva<void*>(kRvaSendSellRequestPacket)) &&
           (gMiSendBuyPacket || AtRva<void*>(kRvaSendBuyRequestPacket));
}

void* GetBagList(int invType) { return x::ui::player::GetItemSlotList(invType); }

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
    // 校验失败当未开。BIN 2026-08-20：getGo 未绑时保守 true → 落地未 Talk 就 ShopReady，
    // 卖栏 listN=0、全是「店不可卖」。
    if (!getGo || !getActive) return false;
    void* go = nullptr;
    __try {
        go = getGo(dlg, gMiGetGameObject);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        go = nullptr;
    }
    if (!go) return false;
    bool active = false;
    __try {
        active = getActive(go, gMiGoGetActiveSelf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        active = false;
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
        *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + kOffArrLen));
    for (int i = 0; i < n && i < 16; ++i) {
        void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
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
        x::runtime::il2cpp::RuntimeClassInit(k);
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
    bool locateOnly = false;
    bool inRangeOnly = false;
    bool ok = false;
    int npcOid = 0;
    int matchedTpl = 0;
    float dist = 0.f;
    float npcX = 0.f;
    float npcY = 0.f;
    float playerX = 0.f;
    float playerY = 0.f;
    int poolN = 0;
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
    job->npcX = 0.f;
    job->npcY = 0.f;
    job->playerX = 0.f;
    job->playerY = 0.f;
    job->poolN = 0;
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
    // 指定模板时全图找该 tpl（BIN 误杀后改 300 导致店图内 no target）。
    // 杂货/药店：玩家双击 NPC 无距离门，TalkToNpc 同样远距可开店（BIN 18:00 銘仁 dist=584 开成）。
    // inRangeOnly 只给船/转职：那些远距 Talk 会被服端踢。无模板时仍用 maxD，避免乱点远处无关 NPC。
    constexpr float kTplMapWide = 8000.f;
    const float tplMaxD2 =
        job->preferTemplateId > 0 ? (kTplMapWide * kTplMapWide) : maxD2;
    const int n = ListSize(list);
    job->poolN = n;
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

    // 指定杂货 tpl 时只对该人 Talk（BIN 2026-08-20：市集落地左侧勿退近距乱点）。
    if (job->preferTemplateId > 0 && !bestTpl) {
        job->err = "no target npc";
        return;
    }
    void* best = bestTpl ? bestTpl : bestNear;
    float bestD2 = bestTpl ? bestTplD2 : bestNearD2;
    int bestOid = bestTpl ? bestTplOid : bestNearOid;
    int bestTplId = bestTpl ? job->preferTemplateId : bestNearTpl;
    if (!best) {
        job->err = job->preferTemplateId > 0 ? "no target npc" : "no nearby npc";
        return;
    }

    float nx = 0.f, ny = 0.f;
    (void)ReadPos2(best, nx, ny);
    job->npcOid = bestOid;
    job->matchedTpl = bestTplId;
    job->dist = std::sqrt(bestD2);
    job->npcX = nx;
    job->npcY = ny;
    job->playerX = px;
    job->playerY = py;

    if (job->locateOnly) {
        job->ok = true;
        job->err = "ok";
        return;
    }
    if (job->inRangeOnly && job->dist > maxD) {
        job->err = "too far";
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
    void* fk = x::runtime::il2cpp::AllocObject(klass);
    if (!LooksLikeHeapPtr(fk)) {
        job->err = "FuncKey alloc";
        return;
    }
    uintptr_t gc = 0;
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
                            *reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(arr) + kOffArrLen))
                      : 0;
    void* dlg = nullptr;
    for (int i = 0; i < n && i < 8; ++i) {
        void* o = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(arr) + kOffArrData +
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
    if (!gMiUiDlgOnClickOk || !gMiUiDlgSelectMenu) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        if (gUiDlgKlass && !gMiUiDlgOnClickOk) {
            constexpr MethodShape kOk{0, TypeKind::Void, true, false, {}};
            gMiUiDlgOnClickOk =
                ResolveMi(gUiDlgKlass, kRvaUiDlgOnClickBtOk, kOk, "OnClickBtOk", nullptr);
        }
        if (gUiDlgKlass && !gMiUiDlgSelectMenu) {
            constexpr MethodShape kMenu{1, TypeKind::Void, false, false, {TypeKind::I32}};
            gMiUiDlgSelectMenu = ResolveMi(gUiDlgKlass, kRvaUiDlgSelectMenu, kMenu, "SetKeyFocus",
                                           kHashSetKeyFocus);
        }
    }
    auto clickOk = FnFromMi<FnUiDlgOnClickOk>(gMiUiDlgOnClickOk, kRvaUiDlgOnClickBtOk);
    auto selectMenu = FnFromMi<FnUiDlgSelectMenu>(gMiUiDlgSelectMenu, kRvaUiDlgSelectMenu);
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
            if (selectMenu) selectMenu(dlg, pick, gMiUiDlgSelectMenu);
            clickOk(dlg, gMiUiDlgOnClickOk);
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
            clickOk(dlg, gMiUiDlgOnClickOk);
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
        const int itemId = ReadI32(slot, x::ui::player::OffSlotItemId());
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

struct ChargeJob {
    int charged = 0;
    int skipMeso = 0;
    int skipOther = 0;
    bool ok = false;
    char err[96]{};
};

// BIN 22:42：空格充完后对 pos=9 slotN=500 连发数十次（金币不变）直至超时弹错。
// 同 pos+qty+meso 连火 ≥kChargeStuckLimit → 本会话拉黑该 pos（多半已满/服拒，投影未刷新）。
constexpr int kChargeStuckLimit = 3;
constexpr int kChargeSkipCap = 32;
int gChargeSkipPos[kChargeSkipCap]{};
int gChargeSkipN = 0;
int gChargeStuckPos = -1;
int gChargeStuckQty = -1;
int64_t gChargeStuckMeso = -1;
int gChargeStuckHits = 0;
// BIN 23:19：开店后首切消耗 TAB 时常 listN=0；若之后不再切 TAB，CmpSellItem
// 会一直空投影 → Charge 死等 LIST_STALE。偶数拍弹到 Etc 再切回 Consume 强制刷新。
int gChargeEmptyStreak = 0;

bool ChargePosIsSkipped(int pos) {
    for (int i = 0; i < gChargeSkipN; ++i) {
        if (gChargeSkipPos[i] == pos) return true;
    }
    return false;
}

void ChargePosSkip(int pos) {
    if (ChargePosIsSkipped(pos) || gChargeSkipN >= kChargeSkipCap) return;
    gChargeSkipPos[gChargeSkipN++] = pos;
    x::runtime::LogW("Shop", "charge skipStuck pos=%d (no meso/qty change ×%d)", pos,
                     kChargeStuckLimit);
}

void NoteChargeFired(int pos, int qty, int64_t meso) {
    if (pos == gChargeStuckPos && qty == gChargeStuckQty && meso == gChargeStuckMeso) {
        ++gChargeStuckHits;
        if (gChargeStuckHits >= kChargeStuckLimit) ChargePosSkip(pos);
    } else {
        gChargeStuckPos = pos;
        gChargeStuckQty = qty;
        gChargeStuckMeso = meso;
        gChargeStuckHits = 1;
    }
}

bool ChargeLooksStuck(int pos, int qty, int64_t meso) {
    return pos == gChargeStuckPos && qty == gChargeStuckQty && meso == gChargeStuckMeso &&
           gChargeStuckHits >= 1;
}

bool IsShurikenItemId(int itemId) {
    return itemId >= kShurikenIdMin && itemId <= kShurikenIdMax;
}

int64_t ReadMesoNow() { return x::ui::player::ReadMoney(); }

// 每次最多充 1 格飞镖：选赤字最大且金币够的卖栏行 → SendRechargeRequestPacket。
void ChargeJobOnMain(void* user) {
    auto* job = reinterpret_cast<ChargeJob*>(user);
    if (!job) return;
    job->ok = false;
    job->charged = 0;
    job->skipMeso = 0;
    job->skipOther = 0;
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
        auto* sendPkt = reinterpret_cast<FnSendRechargePacket>(
            gMiSendRechargePacket && gMiSendRechargePacket->methodPointer
                ? gMiSendRechargePacket->methodPointer
                : AtRva<void*>(kRvaSendRechargeRequestPacket));
        auto* cmpSell = reinterpret_cast<FnCmpSellItem>(
            gMiCmpSellItem && gMiCmpSellItem->methodPointer ? gMiCmpSellItem->methodPointer
                                                           : AtRva<void*>(kRvaCmpSellItem));
        if (!sendPkt) {
            strncpy_s(job->err, "NO_RPC", _TRUNCATE);
            return;
        }
        if ((ReadI32(gShopDlg, kOffHasShopRequestSent) & 0xFF) != 0) {
            strncpy_s(job->err, "SHOP_BUSY", _TRUNCATE);
            return;
        }
        bool tabSwitched = false;
        // 连续空投影：先弹 Etc（仅刷新用，本拍不扫飞镖），下拍再回 Consume。
        const bool bounceEtc =
            gChargeEmptyStreak > 0 && (gChargeEmptyStreak % 4) == 2;
        const int wantInv = bounceEtc ? kInvTiEtc : kInvTiConsume;
        (void)EnsureShopSellInvTab(gShopDlg, wantInv, &tabSwitched);
        if (cmpSell) {
            __try {
                cmpSell(gShopDlg, gMiCmpSellItem);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        void* list = ReadPtr(gShopDlg, kOffSellItemList);
        const int listN = ListSize(list);
        if (bounceEtc || tabSwitched || listN <= 0) {
            if (listN <= 0 || bounceEtc) ++gChargeEmptyStreak;
            strncpy_s(job->err, "LIST_STALE", _TRUNCATE);
            x::runtime::LogW("Shop",
                             "charge LIST_STALE sellListN=%d switched=%d bounce=%d emptyN=%d "
                             "(retry)",
                             listN, tabSwitched ? 1 : 0, bounceEtc ? 1 : 0, gChargeEmptyStreak);
            return;
        }
        gChargeEmptyStreak = 0;
        if (listN > 512) {
            strncpy_s(job->err, "LIST_BAD", _TRUNCATE);
            return;
        }
        const int64_t meso = ReadMesoNow();
        // 读钱失败：禁止乐观发包（买入路径同款 meso>=0 门禁；此处直接收工）
        if (meso < 0) {
            job->ok = true;
            strncpy_s(job->err, "NO_MESO", _TRUNCATE);
            ++job->skipMeso;
            x::runtime::LogW("Shop", "charge meso unread listN=%d → NO_MESO", listN);
            return;
        }
        for (int pickGuard = 0; pickGuard < 32; ++pickGuard) {
        int bestIdx = -1;
        int bestDeficit = 0;
        int64_t bestCost = 0;
        int bestId = 0;
        int bestPos = 0;
        int bestMax = 0;
        int bestQty = 0;
        double bestUnit = 0.0;
        int diagLogged = 0;
        for (int i = 0; i < listN; ++i) {
            void* it = ListAt(list, i);
            if (!LooksLikeHeapPtr(it)) continue;
            const int itemId = ReadI32(it, kOffShopItemId);
            if (!IsShurikenItemId(itemId)) continue;
            const int itemPos = ReadI32(it, kOffShopItemPos);
            if (ChargePosIsSkipped(itemPos)) {
                ++job->skipOther;
                continue;
            }
            // BIN 20:11：卖栏 MaxSlot/Stock/Quantity 常 0；游戏 Charge 读 ItemSlot@0x48
            const int dtoMax = ReadI32(it, kOffShopItemMaxSlot);
            const int stock = ReadI32(it, kOffShopItemStock);
            const int qtyField = ReadI32(it, kOffShopItemQty);
            void* slot = ReadPtr(it, kOffShopItemSlot);
            const int slotN = BundleNumberRaw(slot);
            // 满格：DTO → ItemBundle.nMaxPerSlot / Info.slotMax → 最后才 500
            int maxSlot = dtoMax;
            if (maxSlot <= 0) maxSlot = LookupItemMaxPerSlot(itemId);
            if (maxSlot <= 0) maxSlot = kShurikenDefaultMaxSlot;
            // 当前量优先 Bundle.nNumber（BIN 22:42：Stock 有值时勿盖过 slotN）
            int qty = (slotN >= 0) ? slotN : stock;
            if (qty < 0) qty = qtyField;
            if (slotN < 0 && stock <= 0 && qtyField <= 0) {
                ++job->skipOther;
                if (diagLogged < 3) {
                    ++diagLogged;
                    x::runtime::LogI(
                        "Shop",
                        "charge skipNoSlot id=%d dtoMax=%d stock=%d qty=%d slot=%p unit=%.2f",
                        itemId, dtoMax, stock, qtyField, slot,
                        ReadF64(it, kOffShopItemUnitPrice));
                }
                continue;
            }
            // 一口价 = UintPrice；unit=0 不发包（BIN 23:48：已满格 unit=0 cost=0 空耗）
            const double unit = ReadF64(it, kOffShopItemUnitPrice);
            if (qty >= maxSlot) {
                ++job->skipOther;
                if (diagLogged < 3) {
                    ++diagLogged;
                    x::runtime::LogI(
                        "Shop",
                        "charge skipFull id=%d slotN=%d stock=%d dtoMax=%d max=%d unit=%.2f",
                        itemId, slotN, stock, dtoMax, maxSlot, unit);
                }
                continue;
            }
            if (!(unit > 0.0)) {
                ++job->skipOther;
                if (diagLogged < 3) {
                    ++diagLogged;
                    x::runtime::LogI(
                        "Shop",
                        "charge skipUnit id=%d slotN=%d/%d unit=%.2f",
                        itemId, qty, maxSlot, unit);
                }
                continue;
            }
            const int deficit = maxSlot - qty;
            const int64_t cost = static_cast<int64_t>(unit + 0.5);
            if (cost > meso) {
                ++job->skipMeso;
                if (diagLogged < 3) {
                    ++diagLogged;
                    x::runtime::LogI(
                        "Shop",
                        "charge skipMeso id=%d slotN=%d/%d unit=%.2f cost=%lld meso=%lld",
                        itemId, qty, maxSlot, unit, static_cast<long long>(cost),
                        static_cast<long long>(meso));
                }
                continue;
            }
            const bool better = bestIdx < 0 || cost < bestCost ||
                                (cost == bestCost && deficit > bestDeficit);
            if (better) {
                bestDeficit = deficit;
                bestCost = cost;
                bestIdx = i;
                bestId = itemId;
                bestPos = itemPos;
                bestMax = maxSlot;
                bestQty = qty;
                bestUnit = unit;
            }
        }
        if (bestIdx < 0) {
            job->ok = true;
            if (job->skipMeso > 0)
                strncpy_s(job->err, "NO_MESO", _TRUNCATE);
            else
                strncpy_s(job->err, "NONE", _TRUNCATE);
            x::runtime::LogI("Shop", "charge none skipMeso=%d skipOther=%d listN=%d meso=%lld",
                             job->skipMeso, job->skipOther, listN,
                             static_cast<long long>(meso));
            return;
        }
        // 同格同量同金币已火过 → 拉黑重选，勿再空发（BIN 23:48）
        if (ChargeLooksStuck(bestPos, bestQty, meso)) {
            ChargePosSkip(bestPos);
            ++job->skipOther;
            continue;
        }
        WriteI32(gShopDlg, kOffSellSelectedIndex, bestIdx);
        WriteI32(gShopDlg, kOffLastSellIndex, bestIdx);
        sendPkt(gShopDlg, gMiSendRechargePacket);
        NoteChargeFired(bestPos, bestQty, meso);
        job->charged = 1;
        job->ok = true;
        snprintf(job->err, sizeof(job->err), "FIRED via=ui");
        x::runtime::LogI(
            "Shop",
            "charge FIRED via=ui id=%d pos=%d idx=%d slotN=%d/%d unit=%.2f deficit=%d "
            "cost=%lld meso=%lld listN=%d",
            bestId, bestPos, bestIdx, bestQty, bestMax, bestUnit, bestDeficit,
            static_cast<long long>(bestCost), static_cast<long long>(meso), listN);
        return;
        }  // pickGuard
        job->ok = true;
        strncpy_s(job->err, "NONE", _TRUNCATE);
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
            // 部分货架 Price 明文为 0，单价在 UintPrice(double)；买入规划要求 price>0
            if (outPrice <= 0) {
                const double unit = ReadF64(it, kOffShopItemUnitPrice);
                if (unit > 0.0 && unit < 1.0e9) outPrice = static_cast<int>(unit + 0.5);
            }
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
        int qty = job->count > 0 ? job->count : 1;
        // 货架 Item.MaxSlotCount：箭矢等堆叠上限；一次买超上限会被服端拒而客户端仍 FIRED
        {
            void* list = ReadPtr(gShopDlg, listOff);
            void* row = (listN > 0 && buyIdx >= 0 && buyIdx < listN) ? ListAt(list, buyIdx) : nullptr;
            if (LooksLikeHeapPtr(row)) {
                const int maxSlot = ReadI32(row, kOffShopItemMaxSlot);
                if (maxSlot > 0 && qty > maxSlot) {
                    x::runtime::LogI("Shop", "buy clamp id=%d cnt %d→%d (MaxSlot)", job->itemId, qty,
                                     maxSlot);
                    qty = maxSlot;
                }
            }
        }
        if (price > 0) {
            const int64_t meso = ReadMesoNow();
            if (meso >= 0 && (int64_t)price * (int64_t)qty > meso) {
                strncpy_s(job->err, "NO_MESO", _TRUNCATE);
                return;
            }
        }
        WriteI32(gShopDlg, kOffBuySelectedIndex, buyIdx);
        WriteI32(gShopDlg, kOffLastBuyIndex, buyIdx);
        sendPkt(gShopDlg, qty, gMiSendBuyPacket);
        job->ok = true;
        job->count = qty;  // 回写实发数量，供调用方扣减 need
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
        if (ReadI32(slot, x::ui::player::OffSlotItemId()) != job->itemId) continue;
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
    job->meso = x::ui::player::ReadMoney();
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
    // 与 ScanJob 同口径：TWMS 栏表常 1-based，[0]=空垫。把垫格算进 cap 会永远 used<cap，
    // 满栏自动卖判不满（BIN 14:31 hangup 每轮 no_trip，人眼已满）。
    const bool oneBased = (n > 1 && ListAt(list, 0) == nullptr && ListAt(list, 1) != nullptr);
    const int start = oneBased ? 1 : 0;
    job->cap = n - start;
    int used = 0;
    for (int i = start; i < n; ++i) {
        void* slot = ListAt(list, i);
        if (!LooksLikeHeapPtr(slot)) continue;
        if (ReadI32(slot, x::ui::player::OffSlotItemId()) > 0) ++used;
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

bool LocateNpcByTemplate(int templateId, NpcLocate& out) {
    out = NpcLocate{};
    if (templateId <= 0) return false;
    TalkJob job{};
    job.maxDist = 8000.f;
    job.preferTemplateId = templateId;
    job.locateOnly = true;
    if (!x::runtime::managed_main::Call(&TalkJobOnMain, &job, kJobWaitMs)) return false;
    out.ok = job.ok;
    out.oid = job.npcOid;
    out.tpl = job.matchedTpl;
    out.dist = job.dist;
    out.x = job.npcX;
    out.y = job.npcY;
    out.playerX = job.playerX;
    out.playerY = job.playerY;
    return job.ok;
}

bool TryTalkNearestNpc(float maxDist, int preferTemplateId, bool inRangeOnly) {
    TalkJob job{};
    job.maxDist = maxDist > 1.f ? maxDist : kDefaultTalkDist;
    job.preferTemplateId = preferTemplateId;
    job.inRangeOnly = inRangeOnly;
    if (!x::runtime::managed_main::Call(&TalkJobOnMain, &job, kJobWaitMs)) return false;
    const DWORD now = GetTickCount();
    if (now - gLastTalkLogMs > 2500) {
        gLastTalkLogMs = now;
        x::runtime::LogI("Shop",
                         "TryTalkNearest ok=%d oid=%d tpl=%d want=%d dist=%.1f poolN=%d err=%s",
                         job.ok ? 1 : 0, job.npcOid, job.matchedTpl, preferTemplateId, job.dist,
                         job.poolN, job.err ? job.err : "?");
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

struct SellSnapJob {
    int invType = 0;
    int* outIds = nullptr;
    int maxOut = 0;
    int count = 0;
    int listN = 0;
    bool tabSwitched = false;
    bool ok = false;
};

void SellSnapJobOnMain(void* user) {
    auto* job = reinterpret_cast<SellSnapJob*>(user);
    if (!job) return;
    job->ok = false;
    job->count = 0;
    job->listN = 0;
    job->tabSwitched = false;
    __try {
        gLastRebindMs = 0;
        if (!Rebind(GetTickCount())) return;
        ReadyJob ready{};
        ReadyJobOnMain(&ready);
        if (!ready.ready || !LooksLikeHeapPtr(gShopDlg)) return;

        bool tabSwitched = false;
        (void)EnsureShopSellInvTab(gShopDlg, job->invType, &tabSwitched);
        job->tabSwitched = tabSwitched;
        auto* cmpSell = reinterpret_cast<FnCmpSellItem>(
            gMiCmpSellItem && gMiCmpSellItem->methodPointer ? gMiCmpSellItem->methodPointer
                                                           : AtRva<void*>(kRvaCmpSellItem));
        if (cmpSell) {
            __try {
                cmpSell(gShopDlg, gMiCmpSellItem);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        void* list = ReadPtr(gShopDlg, kOffSellItemList);
        const int n = ListSize(list);
        job->listN = n;
        if (n < 0 || n > 512) {
            job->ok = true;  // 空/异常投影也算快照成功（listN 如实）
            return;
        }
        for (int i = 0; i < n; ++i) {
            void* it = ListAt(list, i);
            if (!LooksLikeHeapPtr(it)) continue;
            const int id = ReadI32(it, kOffShopItemId);
            if (id <= 0) continue;
            if (job->outIds && job->count < job->maxOut) job->outIds[job->count] = id;
            ++job->count;
        }
        job->ok = true;
        if (tabSwitched) {
            x::runtime::LogI("Shop", "sell snap inv=%d listN=%d ids=%d switched=1", job->invType,
                             job->listN, job->count);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        job->ok = false;
    }
}

bool SnapshotShopSellList(int invType, int* outItemIds, int maxOut, int& outCount, int& outListN,
                          bool* outTabSwitched) {
    outCount = 0;
    outListN = 0;
    if (outTabSwitched) *outTabSwitched = false;
    SellSnapJob job{};
    job.invType = invType;
    job.outIds = outItemIds;
    job.maxOut = maxOut > 0 ? maxOut : 0;
    if (!x::runtime::managed_main::Call(&SellSnapJobOnMain, &job, kJobWaitMs)) return false;
    outCount = job.count;
    outListN = job.listN;
    if (outTabSwitched) *outTabSwitched = job.tabSwitched;
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

bool BuyItem(int itemId, int count, std::string& outErr, int* outBought) {
    outErr.clear();
    if (outBought) *outBought = 0;
    BuyJob job{};
    job.itemId = itemId;
    job.count = count;
    if (!x::runtime::managed_main::Call(&BuyJobOnMain, &job, kJobWaitMs)) {
        outErr = "MAIN_TIMEOUT";
        return false;
    }
    outErr = job.err;
    if (outBought && job.ok) *outBought = job.count > 0 ? job.count : 0;
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

struct BuyShelfSnapJob {
    int focusId = 0;
    bool ok = false;
};

void BuyShelfSnapJobOnMain(void* user) {
    auto* job = reinterpret_cast<BuyShelfSnapJob*>(user);
    if (!job) return;
    job->ok = false;
    gLastRebindMs = 0;
    if (!Rebind(GetTickCount())) return;
    ReadyJob ready{};
    ReadyJobOnMain(&ready);
    if (!ready.ready || !LooksLikeHeapPtr(gShopDlg)) {
        x::runtime::LogW("Shop", "buyShelf snap NO_SHOP focus=%d", job->focusId);
        job->ok = true;  // 调用方已知无店；仍算完成
        return;
    }
    const size_t offs[] = {kOffBuyItemList0, kOffBuyItemList1};
    int focusHit = 0;
    int focusPrice = 0;
    double focusUnit = 0.0;
    for (size_t lo : offs) {
        void* list = ReadPtr(gShopDlg, lo);
        const int n = ListSize(list);
        char sample[160]{};
        size_t pos = 0;
        const int show = n > 12 ? 12 : n;
        for (int i = 0; i < show; ++i) {
            void* it = ListAt(list, i);
            if (!LooksLikeHeapPtr(it)) continue;
            const int id = ReadI32(it, kOffShopItemId);
            const int price = ReadI32(it, kOffShopItemPrice);
            const double unit = ReadF64(it, kOffShopItemUnitPrice);
            if (job->focusId > 0 && id == job->focusId) {
                focusHit = 1;
                focusPrice = price;
                focusUnit = unit;
            }
            if (pos + 28 < sizeof(sample)) {
                pos += static_cast<size_t>(
                    snprintf(sample + pos, sizeof(sample) - pos, "%s%d:%d", pos ? "," : "", id, price));
            }
        }
        if (n > show && pos + 8 < sizeof(sample)) {
            snprintf(sample + pos, sizeof(sample) - pos, ",…");
        }
        x::runtime::LogI("Shop", "buyShelf off=0x%zX n=%d sample(id:price)=[%s]", lo, n, sample);
    }
    if (job->focusId > 0) {
        x::runtime::LogI("Shop", "buyShelf focus id=%d hit=%d price=%d unit=%.3f", job->focusId,
                         focusHit, focusPrice, focusUnit);
    }
    job->ok = true;
}

bool LogBuyShelfSnapshot(int focusItemId) {
    BuyShelfSnapJob job{};
    job.focusId = focusItemId;
    if (!x::runtime::managed_main::Call(&BuyShelfSnapJobOnMain, &job, kJobWaitMs)) return false;
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
    bool petFeedMerchant = false;  // 寵物飼料/寵物商人：货架不能卖普通装备
};

constexpr uint32_t kTagSell = 1u;
constexpr uint32_t kTagPotion = 2u;
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
        s.petFeedMerchant = line.find("寵物飼料商人") != std::string::npos ||
                            line.find("寵物商人") != std::string::npos;
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

// 主城户外街图（…000）：卷落地点常找不到「後街」类 NPC（BIN 4bb7ea 吉姆）。
// 加 >1hop 惩罚，让室内杂货/药店在 via=0 vs via=1 时胜出。
bool LooksLikeTownOutdoorMap(int mapId) {
    return mapId >= 100000000 && (mapId % 1000) == 0;
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
    // 有回家卷时：直达优先；仅当学习图直达不可达才用「卷落点→店」估价。
    // 旧 min(直达, via) 会把主城店压成 via=1，洞内近店（如螞蟻礦坑排擋 5~8 跳）永远输给回城。
    bool allowScrollVia = false;
    char scrollTown[16]{};
    if (!cur.empty() && cur != "?") {
        bool present = false;
        int qty = 0;
        auto hasScroll = [&](int itemId) {
            present = false;
            qty = 0;
            return QueryItemPresent(/*consume*/ 2, itemId, present, qty) && present && qty > 0;
        };
        if (hasScroll(2030000) || hasScroll(2030059)) {
            if (features::travel::PredictReturnScrollTownOutdoor(cur.c_str(), scrollTown,
                                                                sizeof(scrollTown))) {
                allowScrollVia = true;
            }
        }
    }

    int bestScore = INT_MAX;
    const GrocerySeed* best = nullptr;
    int bestDirect = -1;
    int bestVia = -1;
    int bestHops = -1;
    bool bestUsedVia = false;

    for (const auto& s : gSeeds) {
        if (excludeMap && excludeMap[0] && MapEqualsLoose(s.mapId, excludeMap)) continue;
        // 科爾等寵物飼料店：BIN 2026-08-20 hops=7 压过露娜雜貨，落地 listN=0 全不可卖。
        if (s.petFeedMerchant) continue;
        int direct = -1;
        int via = -1;
        if (!cur.empty() && cur != "?") {
            direct = features::travel::PathHopCount(cur.c_str(), s.mapId);
            if (direct < 0) direct = -1;
        }
        if (allowScrollVia && scrollTown[0]) {
            via = features::travel::PathHopCount(scrollTown, s.mapId);
            if (via < 0) via = -1;
        }
        int hops = -1;
        bool usedVia = false;
        if (direct >= 0) {
            hops = direct;
        } else if (via >= 0) {
            hops = via;
            usedVia = true;
        } else {
            hops = 9999;  // unreachable → last resort
        }
        // hops 优先；户外主城加罚（BIN 4bb7ea）；同 hops 偏好雜貨/药（potion）压过武器店
        const int mapIdNum = atoi(s.mapId);
        const int outdoorPen = LooksLikeTownOutdoorMap(mapIdNum) ? 15000 : 0;
        const int score = hops * 10000 + outdoorPen + ((s.tags & kTagPotion) ? 0 : 1000) +
                          ((s.tags & kTagSell) ? 0 : 10);
        if (score < bestScore) {
            bestScore = score;
            best = &s;
            bestDirect = direct;
            bestVia = via;
            bestHops = hops;
            bestUsedVia = usedVia;
        }
    }
    if (!best) return false;
    outNpcId = best->npcId;
    outShopId = best->npcId;  // Classic 无独立 shopId；填 npc 便于日志
    outMapName = best->mapId;
    outMapId = atoi(best->mapId);
    x::runtime::LogI(
        "Shop",
        "ResolveShop nearest npc=%s map=%s hops=%d direct=%d via=%s/%d usedVia=%d potion=%d outdoor=%d",
        best->npcId, best->mapId, bestHops == 9999 ? -1 : bestHops, bestDirect,
        allowScrollVia ? scrollTown : "-", bestVia, bestUsedVia ? 1 : 0,
        (best->tags & kTagPotion) ? 1 : 0, LooksLikeTownOutdoorMap(outMapId) ? 1 : 0);
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

void ResetChargeSession() {
    gChargeSkipN = 0;
    gChargeStuckPos = -1;
    gChargeStuckQty = -1;
    gChargeStuckMeso = -1;
    gChargeStuckHits = 0;
    gChargeEmptyStreak = 0;
}

bool RechargeShurikensInOpenShop(int& outCharged, int& outSkippedNoMeso, int& outSkippedOther,
                                 std::string& outErr) {
    outCharged = 0;
    outSkippedNoMeso = 0;
    outSkippedOther = 0;
    outErr.clear();
    ChargeJob job{};
    if (!x::runtime::managed_main::Call(&ChargeJobOnMain, &job, kJobWaitMs)) {
        outErr = "MAIN_TIMEOUT";
        return false;
    }
    outCharged = job.charged;
    outSkippedNoMeso = job.skipMeso;
    outSkippedOther = job.skipOther;
    outErr = job.err;
    // SHOP_BUSY / LIST_STALE / NO_SHOP 等：ok=false，调用方重试或跳过
    return job.ok || job.charged > 0;
}

}  // namespace x::features::ports::shop
