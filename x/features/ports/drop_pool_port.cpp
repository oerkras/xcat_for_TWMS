// drop_pool_port — Classic TWMS DropPool + pet vacuum pickup.
// 防漂移：方法 hash→FindMethodResolved；字段 hash→field_get_offset（dump 常量仅 fallback）；
// 矩形包：ByPet RIP → GA 特征扫 → 末级 RVA。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "drop_pool_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/il2cpp_shape.h"

#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/anchor_lamps.h"
#include "pet_port.h"
#include "world_port.h"
#include "user_pool_port.h"
#include "../../ui/player_vitals.h"
#include "../../../common/xcat_pet_loot.h"
#include "../../../common/xcat_item_catalog.h"
#include "../../runtime/bin_dir.h"

#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::drop {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Unity FindAll → x::runtime::il2cpp::kRvaFindObjectsOfTypeAll（il2cpp_bind.h SSOT）
// Remount 2026-08-06：方法 RVA 普遍 +0x1E70；字段 off 未漂；ACS 类/字段哈希全换
constexpr uint32_t kRvaPetTryPickUpDrop = 0xFBE120;  // remounted 2026-08-06 TDI:1516
// IDA 2026-08-14：Pet.TryPickUpDrop 无外部 E8；桩/本体是两个 methodPointers 槽。
// 原生宠 Tick 走 MI→桩(0xFB4B40) E8 本体(0xF9DE50)；Send/ByPet 仍是 E8，MI 钩打不中。
constexpr uint32_t kRvaPetTryPickStub = 0xFB4B40;
constexpr uint32_t kRvaPetTryPickBody = 0xF9DE50;
constexpr uint32_t kRvaDropTryPickUpDrop = 0xF78FA0;  // remounted 2026-08-06 DropPool.TryPickUpDrop(in Vector2)
constexpr uint32_t kRvaDropTryPickUpDropByPet = 0xF7B100;  // remounted 2026-08-13
constexpr uint32_t kRvaPetGetUpgradePetSkill = 0xF7E5B0;  // remounted 2026-08-06
constexpr uint32_t kRvaPetGetItemSlot = 0xF7D4B0;  // remounted 2026-08-06 · ByPet → ItemSlotPet
constexpr uint32_t kRvaPetIsInExceptionList = 0xF73460;  // remounted 2026-08-06
constexpr uint32_t kRvaPetSendDropPickUp = 0xF7D8E0;  // remounted 2026-08-13 Pet.SendDropPickUpRequest
constexpr uint32_t kRvaPoolSendDropPickUp = 0xF7A820;  // remounted 2026-08-06 DropPool.SendDropPickUpRequest
// ByPet Contains 真源（.rdata，非 CollisionCheck / _rcPet）：
//   int32 offX,offY @ +0 ; float w,h @ +0x10
//   rect = (petPos - (offX,offY), w, h)；原生 (25,10)+(50,60)
//   IDA：ByPet → psubd xmm9,[rip+disp] / movsd xmm0,[rip+disp]；旧死钉仅作末级兜底
constexpr uint32_t kRvaByPetRectPackFallback = 0x55C2700;  // remounted 2026-08-20 · ByPet RIP → 25/10/50/60
constexpr int32_t kNativeRectOffX = 25;
constexpr int32_t kNativeRectOffY = 10;
constexpr float kNativeRectW = 50.f;
constexpr float kNativeRectH = 60.f;
// ByPet 函数体扫描上限（当前 size≈0x22a6）
constexpr size_t kByPetScanMax = 0x2800;

constexpr char kHashPetTryPickUp[] =
    "d759b51eebe52f53d242c297b1b77105265d20e08d2f4b85f3f1d89c75eb609";  // remounted 2026-08-06
constexpr char kHashDropTryPickUp[] =
    "ae858b030b0961088459366aee88d7d4641f2c990a964125ad6dbbc5206ebc5";  // remounted 2026-08-06
constexpr char kHashDropTryPickUpByPet[] =
    "feee53ddc487b913960dba65a0e8290151bcac52e01be71b33063c98e347e50";  // remounted 2026-08-13
constexpr char kHashPetGetUpgradeSkill[] =
    "d5256663308a8347235bf8608be6d3d9f3ef7437678acaeab5455a36cc5bea3";  // remounted 2026-08-06
constexpr char kHashPetGetItemSlot[] =
    "d70e738d1dcf533539e0dd1f3e77ae320cf06fdd34f6d3425a54d2e0f95c204";  // remounted 2026-08-06
constexpr char kHashPetIsInException[] =
    "d0d426b1f11c020e5ed5d856fbb4a4c4ff64825f3087c3caa9806163cc642ac";  // remounted 2026-08-06
constexpr char kHashPetSendDropPickUp[] =
    "ea3bf64925abb6e8362147688ee0319814e0d80007163c7a66e61c0f04a9b1f";  // remounted 2026-08-13
constexpr char kHashPoolSendDropPickUp[] =
    "a4cc4ef180d64c569895b0bd4a6e49530d2ad53075290229160df6e42651587";  // remounted 2026-08-06

constexpr char kDropPoolClass[] =
    "a4a7a4909ad038b1cb1b8bf31f24683c3a4ef9505f0ef45e6aa3b030306f01d";  // remounted 2026-08-06 TDI:1489
constexpr char kDropClass[] =
    "aaf484254dc289719078b655ba76acade4282ea4788da961caca54aec0fc0e3";  // remounted 2026-08-06 TDI:1488
// UserLocal → il2cpp_shape::ResolveUserLocalKlass
constexpr char kCollisionCheckClass[] =
    "ec6fba6087c91a312b7312e79cbb1edcf517cc7cb1151186c76cd74da73af53";  // remounted 2026-08-06 TDI:2446
constexpr char kPetClass[] =
    "d28a31eb6c7bd99e28d0df2e9c907d6091b95789cfb1cfa03404c9709a8ab92";  // remounted 2026-08-06 TDI:1516
constexpr char kUserClass[] =
    "b55b16bb785ad758d4375d173f78195e824184cc2edf99eadc7eead36551193";  // TDI:1560 User（m_apPet/CurPos）
constexpr char kVecCtrlOwnerClass[] =
    "d9aab778a925d77c0ae0b654ad29a8c6dc20a1f4684cffb7e533c336bc6ae5c";  // TDI:1586
constexpr char kVecCtrlClass[] =
    "d7d4003a734229d3b8fd8a969b6a9168c36692d3b039b8824d5d40d2cb4430b";
constexpr char kItemSlotPetClass[] =
    "a1598312dfa6a80eb2b6edc2a75870a90fc7bd064d21f69e68dd41183fb8e98";  // GetItemSlot 返回类型

// —— 字段防漂移：hash + field_get_offset；下列常量仅 dump 验证 fallback（off 未漂）——
constexpr size_t kFbApPet = 0x288;  // 08-13：User Pet[]；0x2B0 已是 Vector2
constexpr size_t kFbWmMyUser = 0x28;
constexpr size_t kFbPoolDict = 0x20;
constexpr size_t kFbPetRc = 0x100;
constexpr size_t kFbPetExceptionList = 0x90;
constexpr size_t kFbVecCtrl = 0x50;
constexpr size_t kFbFieldPos = 0x64;
constexpr size_t kFbCurPos = 0x2B0;  // 08-13：User Vector2；0x240 已是数组
constexpr size_t kFbVcAp = 0x98;  // AbsPos 结构起点；Y = Ap+8
constexpr size_t kFbDropId = 0x30;
constexpr size_t kFbDropOwnerId = 0x34;  // CMS Drop.OwnerId
constexpr size_t kFbDropOwnType = 0x3C;
constexpr size_t kFbDropIsMoney = 0x44;
constexpr size_t kFbDropInfo = 0x48;
constexpr size_t kFbDropPt1 = 0x20;
constexpr size_t kFbDropEndPara = 0x7C;
constexpr size_t kFbDropLastTry = 0x80;
constexpr size_t kFbDropPickStamp = 0x88;
// 玩家拾取路径判定用的 Point，与 Pt1@0x20 是两个字段；矩形比的是这一个
constexpr size_t kFbDropPickPt = 0x98;
// bool；为假时官方 TryPickUpDrop 当场跳过该 drop
constexpr size_t kFbDropPickable = 0x2D;
constexpr size_t kFbItemSlotPetSkill = 0x3C;
constexpr size_t kFbCollisionRcPet = 0x20;

constexpr char kHashFldApPet[] =
    "aa45ac729b476b6bd0b345d43ec2d3d84330d1f1c0b0cbcf45ad41a2573cf40";
constexpr char kHashFldWmMyUser[] =
    "<aa14627d9f4fe9d3642086a9bdb75516742da955d24531087cfb70e3b795d4e>k__BackingField";  // WM.MyUser@0x28
constexpr char kHashFldPoolDict[] =
    "c04f655d960002d39205527433e5a6951ee2028fafb595b6cba62da83ab4a23";
constexpr char kHashFldPetRc[] =
    "c40c48d2ab4af850cb65acb1a122c96571f59b5f7258f0ab48e156b47a0b610";
constexpr char kHashFldPetExceptionList[] =
    "<bdde267dc8a908331e25e36d37ef1525dc422c43b2eabf9bcc4be983ecae8cf>k__BackingField";
constexpr char kHashFldVecCtrl[] =
    "<aeb819450fbe3e8e0eb38423605993f53e2c72baef2b39f45a89237951f1628>k__BackingField";
constexpr char kHashFldFieldPos[] =
    "f5e96097bcfbc4e0b6bb1606c0cc3f2e20f2635a65745766422d9c9b50e0386";
constexpr char kHashFldCurPos[] =
    "ae4a30c4aa075fb68238dc227c1799d252632cad9320bf76370521351096d27";
constexpr char kHashFldVcAp[] =
    "c58e00a053bb88a5ed4a0a369ce0968c883a9ff77b788c812b896dc6c58aca3";
constexpr char kHashFldDropId[] =
    "e53078201350a0a1812518f1268b69076a8ed1087f285dbce1fc8f8bccdbee4";
constexpr char kHashFldDropOwnType[] =
    "e511f1df1250087bd8a0585df3f43c5cd170dd0b58d9a3e6f90e2b4713076b1";
constexpr char kHashFldDropIsMoney[] =
    "ce3ebb09a40cddccf3dbc8a3f713b11f34de86aa90dc9fd853775541f54289e";
constexpr char kHashFldDropInfo[] =
    "a5edc93887aa73edcd1b8befbed43b74e3b2e2d0a529c5879612d5a83230fdd";
constexpr char kHashFldDropPt1[] =
    "cd5a8b72fd1b5d7f4dbb0f2246e0e2c2b2163ec9baa8f58cd68fcf530a6f1b5";
constexpr char kHashFldDropEndPara[] =
    "af8cc709d41f76d7baa5985dc966e3cbb53af53adfbb4ad070f0ac24aca3843";
constexpr char kHashFldDropLastTry[] =
    "abee4318e4419615f01fc5dc7ceb6a2d6affe4f84a89477ba58beb0af0af93d";
constexpr char kHashFldDropPickStamp[] =
    "db24c364144a0b975421aee6ca7ca8569d171c71375d8cb8d4efdbeaa7a2392";
constexpr char kHashFldDropPickPt[] =
    "ff9e2dcef503a3214fb9434edb16af6ddce171244b57b0b042f257c2b47c6c0";
constexpr char kHashFldDropPickable[] =
    "d91536fd1457b83cd58158077e1faa7327f3543f092564c78f14febeb5ad9d7";
constexpr char kHashFldItemSlotPetSkill[] =
    "c1001e2605753cfeb2e08aeca0d3c1274181f74536904dfef4fe2f2514deda2";
constexpr char kHashFldCollisionRcPet[] =
    "aff0b7e4e799fc7a9ecfbd433b250f7750c0bfd77e2f0fefbbae1b5dda4e249";

struct DropFieldOff {
    size_t apPet = kFbApPet;
    size_t wmMyUser = kFbWmMyUser;
    size_t poolDict = kFbPoolDict;
    size_t petRc = kFbPetRc;
    size_t petExceptionList = kFbPetExceptionList;
    size_t vecCtrl = kFbVecCtrl;
    size_t fieldPos = kFbFieldPos;
    size_t curPos = kFbCurPos;
    size_t vcAp = kFbVcAp;
    size_t dropId = kFbDropId;
    size_t dropOwnerId = kFbDropOwnerId;
    size_t dropOwnType = kFbDropOwnType;
    size_t dropIsMoney = kFbDropIsMoney;
    size_t dropInfo = kFbDropInfo;
    size_t dropPt1 = kFbDropPt1;
    size_t dropEndPara = kFbDropEndPara;
    size_t dropLastTry = kFbDropLastTry;
    size_t dropPickStamp = kFbDropPickStamp;
    size_t dropPickPt = kFbDropPickPt;
    size_t dropPickable = kFbDropPickable;
    size_t itemSlotPetSkill = kFbItemSlotPetSkill;
    size_t collisionRcPet = kFbCollisionRcPet;
    // 只在全解析后锁定；否则下拍重试，别让一次早期抖动把整局钉在死钉上
    bool tried = false;
    // 写路径门禁：写决策依赖的 Drop 字段 + 宠 ExceptionList 必须都由元数据解析。
    // 本 port 会写 EndPara/LastTry/PickStamp/OwnType，错址写是踩托管堆，比「本拍不吸」严重得多。
    bool writeSafe = false;
    DWORD nextTry = 0;
    int loggedHits = -1;
    const char* path = "fallback";
    int hits = 0;
};
DropFieldOff gOff{};

// 写托管 Drop 字段是否安全（字段偏移全部来自元数据）
bool DropWritesAllowed() { return gOff.writeSafe; }

// 热路径仍用旧名；EnsureFieldOffsets 后指向 meta/fallback。
#define kOffApPet (gOff.apPet)
#define kOffWmMyUser (gOff.wmMyUser)
#define kOffPoolDict (gOff.poolDict)
#define kOffPetRc (gOff.petRc)
#define kOffPetExceptionList (gOff.petExceptionList)
#define kOffVecCtrl (gOff.vecCtrl)
#define kOffFieldPos (gOff.fieldPos)
#define kOffCurPos (gOff.curPos)
#define kOffVcApX (gOff.vcAp)
#define kOffVcApY (gOff.vcAp + 8)
#define kOffDropId (gOff.dropId)
#define kOffDropOwnerId (gOff.dropOwnerId)
#define kOffDropOwnType (gOff.dropOwnType)
#define kOffDropIsMoney (gOff.dropIsMoney)
#define kOffDropInfo (gOff.dropInfo)
#define kOffDropPt1 (gOff.dropPt1)
#define kOffDropEndPara (gOff.dropEndPara)
#define kOffDropLastTry (gOff.dropLastTry)
#define kOffDropPickStamp (gOff.dropPickStamp)
#define kOffDropPickPt (gOff.dropPickPt)
#define kOffDropPickable (gOff.dropPickable)
#define kOffItemSlotPetSkill (gOff.itemSlotPetSkill)
#define kOffCollisionRcPet (gOff.collisionRcPet)

// DropOwnType（CMS）：User=0 Party=1 No=2 ExplosiveNoOwn=3 UserOwnMoney=4
constexpr int kDropOwnNo = 2;
// 定义见 ReadI32 之后（依赖堆读 + world::GetCharacterId）
bool DropClientPickable(void* drop);

struct OwnSkipStamp {
    void* drop = nullptr;
    int prevLast = 0;
};
std::vector<OwnSkipStamp> gOwnSkipStamps;

// IDA ByPet：cmp [drop+0x7C], 3 / cmovz 才继续；写 0 会重置抛物线（近图飞落根因）
constexpr int kEndParaReady = 3;
// 黑名单挡 ByPet：必须 !=3；禁止写 0（会重置抛物线飞落）
constexpr int kEndParaSkipHold = 4;
constexpr int kLastTrySkipStamp = 0x7FFFFFFF;

// 人物直吸（不靠宠）：复刻官方 DropPool.TryPickUpDrop（RVA 0xF78FA0）的门禁语义，然后直接调
// 它自己的 DropPool.SendDropPickUpRequest（RVA 0xF7A820）。控制面与宠吸同构（清闸/退避盖戳/
// 黑名单盖戳/拒收即 AddStall/拍末还原），只是中心从宠坐标换成角色坐标、送包走人物入口。
// 官方那条链逐指令实读所得：
//   ① [drop+0x2D] 必须为真                       ② EndPara(0x7C) == 3
//      （IMM 0x328634BB + seed@0x7FFB8A2C92A8=0xCD79CB48 → 3）
//   ③ now - PickStamp(0x88) >= 3000（有符号）——宠吸同款拍前清闸后由官方 Send 再盖
//   ④ 归属：客户端先跳过非己/非无主；服端仍裁决 + Stall 兜底
//   ⑤ 矩形：宠吸扩 ByPet 包；人物直吸用 vacuum 半盒枚举（用户自调，默认 1000×1000）
//      —— 送包坐标仍是角色位；服端距离校验失败则空 Send，不再代用户缩盒
//   通过后 Point(角色 Maple 坐标) → Send(pt, Id, 0)
//      （第 4 参 IMM 0x353E87DA + seed@…92B4=0xCAC17826 → 0，无需伪造 CRC）


// 退避黑名单（队头堵塞解药）：
// ByPet 一次调用只提交一件（实机 sendTouch 恒为 1）。某栏（如「其他」）满时服端拒收，
// drop 留在池里；我们每拍清闸又把它放回队头 → 每拍都撞同一件，金币/装备一起吸不到。
// 处置：提交后池未掉 → 把该 dropId 指数退避，期间盖 LastTry=INT_MAX 让 ByPet 早退，
// 下一拍自然轮到别的掉落；到期自动解封（用户整理背包后仍能重吸），换池即全清。
constexpr DWORD kStallBaseMs = 3000;
constexpr DWORD kStallMaxMs = 300000;
constexpr size_t kStallCap = 2048;

struct StallEntry {
    DWORD until = 0;
    int strikes = 0;  // 连续被拒次数 → 退避时长翻倍
};
std::unordered_map<int, StallEntry> gStall;
void* gStallPool = nullptr;
// Drop.Id 不唯一（偏移漂移）时自动停用：宁可退回原行为，也不能把全盒挡死
bool gStallOff = false;
// 人物直吸：送包当帧池常未掉，立刻 AddStall 会误伤真吸。记下 id，下一拍仍在池再登记。
int gCharPendingStallId = 0;

void ClearFlyHolds();  // 飞物软挡表；换池与 Stall 一并清

bool StallExpired(const StallEntry& e, DWORD now) {
    return static_cast<int32_t>(now - e.until) >= 0;
}

// 超量时只淘汰「已过期」条目（丢的仅是 strikes 记账），生效中的 hold 一律保留。
// 若连生效条目都超过上限才整表清空——那已是异常量级。
void PruneStall(DWORD now) {
    if (gStall.size() <= kStallCap) return;
    for (auto it = gStall.begin(); it != gStall.end();) {
        if (StallExpired(it->second, now))
            it = gStall.erase(it);
        else
            ++it;
    }
    if (gStall.size() > kStallCap) gStall.clear();
}

// 换图/换池后 dropId 会复用，旧退避必须失效。
// 只认「换到另一个非空池」；池临时解析不到时保留退避，免得每次抖动都重新学一遍。
void ResetStallIfPoolChanged(void* pool) {
    if (!pool || pool == gStallPool) return;
    gStallPool = pool;
    gStall.clear();
    gCharPendingStallId = 0;
    ClearFlyHolds();
}

bool StallActive(int dropId, DWORD now) {
    if (gStall.empty() || dropId == 0) return false;
    auto it = gStall.find(dropId);
    return it != gStall.end() && !StallExpired(it->second, now);
}

void AddStall(int dropId, DWORD now) {
    if (gStallOff || dropId == 0) return;
    StallEntry& e = gStall[dropId];
    if (e.strikes < 30) ++e.strikes;
    DWORD ms = kStallBaseMs;
    for (int i = 1; i < e.strikes && ms < kStallMaxMs; ++i) ms <<= 1;
    if (ms > kStallMaxMs) ms = kStallMaxMs;
    e.until = now + ms;
}

void ClearStallId(int dropId) {
    if (dropId == 0 || gStall.empty()) return;
    gStall.erase(dropId);
}

int StallActiveCount(DWORD now) {
    int n = 0;
    for (const auto& kv : gStall)
        if (!StallExpired(kv.second, now)) ++n;
    return n;
}
// BCL Dictionary / List → x::runtime::il2cpp_container（meta；常量仅 fallback）
#define kOffDictEntries (x::runtime::il2cpp_container::OffDictEntries())
#define kOffDictCount (x::runtime::il2cpp_container::OffDictCount())
#define kOffDictFreeCount (x::runtime::il2cpp_container::OffDictFreeCount())
#define kEntrySize (x::runtime::il2cpp_container::DictEntryStrideIntPtr())
#define kOffEntryHash (x::runtime::il2cpp_container::OffDictEntryHash())
#define kOffEntryValue (x::runtime::il2cpp_container::OffDictEntryValuePtr())

#define kOffListItems (x::runtime::il2cpp_container::OffListItems())
#define kOffListSize (x::runtime::il2cpp_container::OffListSize())

constexpr DWORD kRebindMs = 3000;
constexpr DWORD kJobWaitMs = 1500;
constexpr float kMinPosAbs = 1.0f;

using FnFindAll = void* (*)(void* typeObj, void* methodInfo);
using FnClassGetMethods = void* (*)(void* klass, void** iter);
using FnClassStaticData = void* (*)(void* klass);
using FnClassParent = void* (*)(void* klass);
using FnRuntimeClassInit = void (*)(void* klass);
using FnCompGo = void* (*)(void* comp, void* methodInfo);
using FnObjName = void* (*)(void* go, void* methodInfo);
using FnPetTryPickUp = void (*)(void* pet, const void* methodInfo);
using FnDropTryPickUp = void (*)(void* pool, const float* posXy /* in Vector2 */, const void* methodInfo);
using FnPetGetSkill = uint16_t (*)(void* pet, const void* methodInfo);
using FnPetGetItemSlot = void* (*)(void* pet, const void* methodInfo);
using FnPetInEx = bool (*)(void* pet, int itemId, const void* methodInfo);
// ByPet 对 Pet.Send 多为直接 call；MI swap 只捕走 methodPointer 的调用。dropsΔ 才是硬证据。
using FnPetSend = bool(__fastcall*)(void* self, uint64_t ptPacked, int dropId, uint32_t crc1,
                                    uint32_t crc2, const void* methodInfo);
using FnPoolSend = void(__fastcall*)(void* self, const void* ptIn, int dropId, uint32_t crc,
                                     const void* methodInfo);

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

HMODULE gGA = nullptr;
FnFindAll gFindAll = nullptr;
FnClassGetMethods gClassGetMethods = nullptr;
FnClassStaticData gClassStaticData = nullptr;
FnClassParent gClassParent = nullptr;
FnRuntimeClassInit gRuntimeClassInit = nullptr;
FnCompGo gCompGo = nullptr;
FnObjName gObjName = nullptr;

void* gDropPoolKlass = nullptr;
void* gDropPool = nullptr;
void* gLuType = nullptr;
void* gLocalUser = nullptr;
void* gPetKlass = nullptr;
void* gCollisionKlass = nullptr;
MethodInfoHead* gMiTryPickUp = nullptr;
MethodInfoHead* gMiFootTryPickUp = nullptr;
MethodInfoHead* gMiGetSkill = nullptr;
MethodInfoHead* gMiGetItemSlot = nullptr;
MethodInfoHead* gMiInEx = nullptr;
MethodInfoHead* gMiPetSend = nullptr;
MethodInfoHead* gMiPoolSend = nullptr;
void* gOrigPetSend = nullptr;
void* gOrigPoolSend = nullptr;
void* gOrigTryPickUp = nullptr;  // ResolveMi 那份原生入口；真空直调，不进钩
struct TryPickMiSlot {
    MethodInfoHead* mi = nullptr;
    void* orig = nullptr;
};
constexpr int kTryPickMiCap = 4;
TryPickMiSlot gTryPickMi[kTryPickMiCap]{};
int gTryPickMiN = 0;
constexpr int kTryPickVtCap = 8;
void** gTryPickVtSlot[kTryPickVtCap]{};
void* gTryPickVtOrig[kTryPickVtCap]{};
int gTryPickVtN = 0;
std::atomic<bool> gTryPickProbeInstalled{false};
std::atomic<uint32_t> gTryPickHookHits{0};
// ByPet .rdata 矩形包：xref/特征定位缓存（拍内 patch 前必须已 resolve）
uint8_t* gByPetRectPack = nullptr;
const char* gByPetRectVia = nullptr;
struct RectHoldState {
    bool active = false;
    uint8_t* base = nullptr;
    DWORD oldProtect = 0;
    float w = 0.f;
    float h = 0.f;
};
RectHoldState gRectHold{};
std::atomic<uint32_t> gPetSendHits{0};
std::atomic<uint32_t> gPoolSendHits{0};
std::atomic<uint32_t> gSkipSwallowHits{0};
std::atomic<bool> gSendProbeInstalled{false};
std::atomic<DWORD> gRejectBackoffUntil{0};
// 拒收连击整段休眠截止 tick（worker 侧可读，避免退避期仍排队 MainPump）
SkipIds gLiveSkipA{};
SkipIds gLiveSkipB{};
std::atomic<int> gLiveSkipSel{0};

void PublishLiveSkip(const SkipIds* skip) {
    const int next = 1 - gLiveSkipSel.load(std::memory_order_relaxed);
    SkipIds& dst = (next == 1) ? gLiveSkipB : gLiveSkipA;
    if (skip)
        dst = *skip;
    else
        dst.clear();
    gLiveSkipSel.store(next, std::memory_order_release);
}

const SkipIds& LiveSkip() {
    return gLiveSkipSel.load(std::memory_order_acquire) ? gLiveSkipB : gLiveSkipA;
}

bool StampOneSkipDrop(void* drop);
void* FindDropById(void* pool, int dropId);
bool DropMatchesSkip(void* drop, const SkipIds& skip);
int StampSkippedDropsNear(void* pool, float cx, float cy, float halfW, float halfH,
                          const SkipIds& skip, int* outNear, int* outWant, int* outTotal);
bool EnsureExceptionIdsIfNeeded(void* pet, const SkipIds& skip);
void EnsureTryPickProbe();

bool ShouldSwallowPickup(int dropId) {
    const SkipIds& skip = LiveSkip();
    if (skip.empty() || dropId <= 0) return false;
    void* pool = gDropPool;
    if (!pool) return false;
    void* drop = FindDropById(pool, dropId);
    if (!drop) return false;
    if (!DropMatchesSkip(drop, skip)) return false;
    StampOneSkipDrop(drop);
    const uint32_t n = gSkipSwallowHits.fetch_add(1, std::memory_order_relaxed) + 1;
    static DWORD sSwLog = 0;
    const DWORD now = GetTickCount();
    if (!sSwLog || now - sSwLog >= 2000u) {
        sSwLog = now;
        x::runtime::LogI("droppool", "skip-swallow send dropId=%d n=%u (land-frame gate)", dropId, n);
    }
    return true;
}

constexpr int kInvTiEquip = 1;
constexpr int kInvTiConsume = 2;
constexpr int kInvTiInstall = 3;
constexpr int kInvTiEtc = 4;

enum class HvClass : int { None = 0, Equip = 1, Scroll = 2, Dart = 3 };

bool InvHasFreeSlot(int invType);
bool CountBagItem(int itemId, unsigned long long& out);
HvClass ClassifyHighValueItem(int info, bool isMoney);
bool HighValueBagAllows(HvClass hv);
bool ReadUserPos(float& x, float& y);
// 可吸 HV = 宠真空 ∩ 角色半盒 ∩ 栏未满；可选填首件样本
void ScanHighValueNear(void* pool, float petX, float petY, float halfW, float halfH, float ux,
                       float uy, float charHalfW, float charHalfH, const SkipIds* skip,
                       int& outNearHv, int& outSkippedFull, int* outSampleDropId = nullptr,
                       int* outSampleInfo = nullptr, int* outSampleKind = nullptr);

const char* HvClassName(HvClass hv) {
    switch (hv) {
    case HvClass::Equip:
        return "equip";
    case HvClass::Scroll:
        return "scroll";
    case HvClass::Dart:
        return "dart";
    default:
        return "none";
    }
}

const char* LookupItemNameBrief(int info) {
    if (info <= 0) return "";
    char code[16]{};
    snprintf(code, sizeof(code), "%d", info);
    const xcat::ItemCatalogPack& pack = xcat::GetSharedItemCatalog(x::runtime::GetBinDir());
    const char* name = xcat::ItemCatalogLookupName(pack, code);
    return (name && name[0]) ? name : "";
}

DWORD gLastLuRebind = 0;
DWORD gLastPoolRebind = 0;

struct VacJob {
    float vacuumW = 300.f;
    float vacuumH = 200.f;
    bool highValuePriority = false;
    SkipIds skip{};
    VacuumResult result{};
    bool done = false;
};

struct FootJob {
    FootResult result{};
    bool done = false;
};

struct CharVacJob {
    float halfW = 750.f;
    float halfH = 750.f;
    int maxSend = 1;
    SkipIds skip{};
    CharVacResult result{};
    bool done = false;
};

std::atomic<bool> gJobPending{false};
VacJob gJob{};
std::atomic<bool> gFootPending{false};
FootJob gFoot{};
CharVacJob gCharVac{};
std::atomic<uint32_t> gCharSentTotal{0};

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

uint8_t ReadU8(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(obj) + off);
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

double ReadF64(void* obj, size_t off) {
    if (!obj) return 0.0;
    __try {
        return *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
}

void WriteF32(void* obj, size_t off, float v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteF64(void* obj, size_t off, double v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<double*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
        *(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// 黑名单长期戳。原生 TryPickUpDrop 看 Pickable@0x2D，不看 EndPara；
// 只盖 LastTry/EndPara 拦不住原生宠。飞行中也写成 SkipHold(4)，禁止再变回 Ready(3)。
// 禁写 EndPara=0。返回是否本拍新动过。
bool StampOneSkipDrop(void* drop) {
    if (!drop || !DropWritesAllowed()) return false;
    bool touched = false;
    if (ReadI32(drop, kOffDropLastTry) != kLastTrySkipStamp) {
        WriteI32(drop, kOffDropLastTry, kLastTrySkipStamp);
        touched = true;
    }
    const int endp = ReadI32(drop, kOffDropEndPara);
    if (endp != kEndParaSkipHold) {
        WriteI32(drop, kOffDropEndPara, kEndParaSkipHold);
        touched = true;
    }
    if (kOffDropPickable >= 0x10 && kOffDropPickable < 0x100 &&
        ReadU8(drop, kOffDropPickable) != 0) {
        WriteU8(drop, kOffDropPickable, 0);
        touched = true;
    }
    if (touched) {
        static DWORD sLog = 0;
        const DWORD now = GetTickCount();
        if (!sLog || now - sLog >= 2000u) {
            sLog = now;
            const int info = ReadI32(drop, kOffDropInfo);
            x::runtime::LogI("droppool",
                             "skip-stamp info=%d endp=%d→4 pickable=0 last=INT_MAX", info, endp);
        }
    }
    return touched;
}

int RestoreOwnSkipStamps() {
    int n = 0;
    if (DropWritesAllowed()) {
        for (const auto& e : gOwnSkipStamps) {
            if (!LooksLikeHeapPtr(e.drop)) continue;
            if (ReadI32(e.drop, kOffDropLastTry) != kLastTrySkipStamp) continue;
            WriteI32(e.drop, kOffDropLastTry, e.prevLast);
            ++n;
        }
    }
    gOwnSkipStamps.clear();
    return n;
}

// 客户端预筛「己 / 无主」——反转口径（2026-08-12）：
//   挡「OwnerId ∈ 远程玩家 ID 集合」；独图（remotes=0）不挡归属。
// BIN：Drop.OwnerId=118536 ≠ CS+0x10=194899，禁止用错槽硬否决己物。
// 真吸认亲仍作多人兜底；热路径禁止 SampleRemoteUserCount/InvokeAndWait。
uint32_t gDropSelfOwnerId = 0;
uint32_t gCharPendingLearnOwner = 0;

constexpr DWORD kRemoteOwnerRefreshMs = 400;
constexpr int kRemoteOwnerCap = 96;
uint32_t gRemoteOwnerIds[kRemoteOwnerCap]{};
int gRemoteOwnerN = 0;
int gRemoteCountCached = 0;
bool gRemotePeekOk = false;
DWORD gRemoteOwnerRefreshAt = 0;
size_t gRemoteDropOwnerOff = 0;  // 远程 User 上命中过 Drop.OwnerId 的偏移（学到才用）

bool PlausibleDropCid(uint32_t id) { return id != 0 && id < 0x04000000u; }

bool ScanObjU32(void* obj, size_t begin, size_t end, uint32_t want, size_t* outOff) {
    if (!LooksLikeHeapPtr(obj) || begin >= end) return false;
    for (size_t off = begin; off + 4 <= end; off += 4) {
        if (static_cast<uint32_t>(ReadI32(obj, off)) == want) {
            if (outOff) *outOff = off;
            return true;
        }
    }
    return false;
}

void ClearRemoteOwnerCache() {
    gRemoteOwnerN = 0;
    gRemoteCountCached = 0;
    gRemotePeekOk = false;
    gRemoteOwnerRefreshAt = 0;
}

void ClearDropSelfOwnerCache() {
    gDropSelfOwnerId = 0;
    gCharPendingLearnOwner = 0;
    ClearRemoteOwnerCache();
    gRemoteDropOwnerOff = 0;
    x::features::ports::world::ClearDropOwnerWmFieldOff();
}

void PushRemoteOwnerId(uint32_t id) {
    if (!PlausibleDropCid(id)) return;
    if (gDropSelfOwnerId && id == gDropSelfOwnerId) return;
    for (int i = 0; i < gRemoteOwnerN; ++i) {
        if (gRemoteOwnerIds[i] == id) return;
    }
    if (gRemoteOwnerN >= kRemoteOwnerCap) return;
    gRemoteOwnerIds[gRemoteOwnerN++] = id;
}

void CollectRemoteUserDropIds(void* user) {
    if (!LooksLikeHeapPtr(user)) return;
    // 远程 Drop.OwnerId 与本地一样不是 User+0x1B0（该槽常≈CS 系）。
    // 只读「真吸/外物扫描」学到的槽；未学到则靠 allowOwned 的 myId 严格分支。
    if (gRemoteDropOwnerOff)
        PushRemoteOwnerId(static_cast<uint32_t>(ReadI32(user, gRemoteDropOwnerOff)));
}

// 若 foreign OwnerId 落在某远程 User 上，记住偏移（下次刷新直接读）。
void MaybeLearnRemoteDropOwnerOff(uint32_t foreignId) {
    if (gRemoteDropOwnerOff || !PlausibleDropCid(foreignId)) return;
    if (gDropSelfOwnerId && foreignId == gDropSelfOwnerId) return;
    void* users[64]{};
    int n = 0;
    if (!x::features::ports::user_pool::PeekEnumRemoteUsers(users, 64, &n) || n <= 0) return;
    for (int i = 0; i < n; ++i) {
        size_t off = 0;
        if (!ScanObjU32(users[i], 0x10, 0x400, foreignId, &off)) continue;
        gRemoteDropOwnerOff = off;
        PushRemoteOwnerId(foreignId);
        x::runtime::LogI("DropPort", "Drop.OwnerId remote slot User+0x%zx (sample=%u remotes=%d)",
                         off, foreignId, n);
        return;
    }
}

void RefreshRemoteDropOwnersIfDue() {
    const DWORD now = GetTickCount();
    if (gRemoteOwnerRefreshAt && now - gRemoteOwnerRefreshAt < kRemoteOwnerRefreshMs) return;
    gRemoteOwnerRefreshAt = now;
    gRemoteOwnerN = 0;
    gRemoteCountCached = 0;
    gRemotePeekOk = false;

    int remotes = 0;
    if (!x::features::ports::user_pool::PeekRemoteUserCount(&remotes)) return;
    gRemotePeekOk = true;
    gRemoteCountCached = remotes;
    if (remotes <= 0) return;

    void* users[64]{};
    int n = 0;
    if (!x::features::ports::user_pool::PeekEnumRemoteUsers(users, 64, &n) || n <= 0) return;
    for (int i = 0; i < n; ++i) CollectRemoteUserDropIds(users[i]);
}

bool IsRemoteDropOwner(uint32_t ownerId) {
    if (!PlausibleDropCid(ownerId) || gRemoteOwnerN <= 0) return false;
    for (int i = 0; i < gRemoteOwnerN; ++i) {
        if (gRemoteOwnerIds[i] == ownerId) return true;
    }
    return false;
}

// OwnType：ByPetParity 死钉 +0x3C（cmp User=0 解混淆常量）；哈希常误钉到 Id/SourceId → 日志百万级。
int ReadDropOwnType(void* drop) {
    if (!LooksLikeHeapPtr(drop)) return -1;
    const int v = ReadI32(drop, kFbDropOwnType);
    if (v >= 0 && v <= 4) return v;
    return -1;
}

int ReadDropOwnRaw(void* drop) {
    if (!LooksLikeHeapPtr(drop)) return 0;
    return ReadI32(drop, kFbDropOwnType);
}

// Ready 落地物：dump +0x30..+0x48 + 扫描 0..4 候选槽（节流）
void MaybeProbeDropOwnLayout(void* drop, int endPara) {
    if (!LooksLikeHeapPtr(drop) || endPara != kEndParaReady) return;
    static DWORD s_lastMs = 0;
    static int s_count = 0;
    if (s_count >= 12) return;
    const DWORD now = GetTickCount();
    if (s_lastMs && now - s_lastMs < 1200u) return;
    s_lastMs = now;
    ++s_count;

    const int id = ReadI32(drop, kFbDropId);
    const int owner = ReadI32(drop, kFbDropOwnerId);
    const int src = ReadI32(drop, 0x38);
    const int o3c = ReadI32(drop, kFbDropOwnType);
    const int o40 = ReadI32(drop, 0x40);
    const int o48 = ReadI32(drop, 0x48);
    const unsigned b2d = ReadU8(drop, kFbDropPickable);
    const unsigned b40 = ReadU8(drop, 0x40);
    const unsigned b44 = ReadU8(drop, kFbDropIsMoney);

    char cand[128];
    int cn = 0;
    cand[0] = '\0';
    for (size_t off = 0x2C; off <= 0x50 && cn + 16 < (int)sizeof(cand); off += 4) {
        const int v = ReadI32(drop, off);
        if (v < 0 || v > 4) continue;
        cn += snprintf(cand + cn, sizeof(cand) - cn, "%s0x%zx=%d", cn ? " " : "", off, v);
        if (cn < 0 || cn >= (int)sizeof(cand)) {
            cand[sizeof(cand) - 1] = '\0';
            break;
        }
    }
    x::runtime::LogI(
        "DropPort",
        "Drop.OwnProbe ready id=%d owner=%d +30=%d +34=%d +38=%d +3C=%d +40=%d +40b=%u +44b=%u "
        "real=%u +48=%d cand[%s]",
        id, owner, id, owner, src, o3c, o40, b40, b44, b2d, o48, cand[0] ? cand : "-");
}

void NoteDropSelfOwnerFromPickup(uint32_t ownerId) {
    if (!PlausibleDropCid(ownerId)) return;
    if (gDropSelfOwnerId == ownerId) return;
    if (gDropSelfOwnerId != 0 && gDropSelfOwnerId != ownerId) {
        x::runtime::LogW("DropPort", "Drop.OwnerId self relearn %u → %u", gDropSelfOwnerId, ownerId);
    }
    gDropSelfOwnerId = ownerId;
    x::runtime::LogI("DropPort", "Drop.OwnerId self=%u (learned from pickup; cs+0x10=%u)", ownerId,
                     x::features::ports::world::GetCharacterId());
    // 本机 User 上若有同值，记偏移供远程对称读取（可能扫不到——BIN 本地常无此槽）
    size_t off = 0;
    void* mu = gLocalUser ? gLocalUser : x::ui::player::LocalMyUser();
    if (!gRemoteDropOwnerOff && ScanObjU32(mu, 0x10, 0x400, ownerId, &off)) {
        gRemoteDropOwnerOff = off;
        x::runtime::LogI("DropPort", "Drop.OwnerId local User+0x%zx (=self)", off);
    }
}

uint32_t LocalDropSelfOwnerId(uint32_t ownerHint) {
    const uint32_t cs10 = x::features::ports::world::GetCharacterId();

    // CS 毒：缓存被写成 CS+0x10，但地上 Drop.OwnerId 是另一套 ID（例 118536≠194899）。
    // 本角若 Drop.OwnerId 本来就等于 CS（例 195466==cs），不得清——否则 myCid 永 0、认亲打转。
    if (gDropSelfOwnerId && cs10 && gDropSelfOwnerId == cs10 && PlausibleDropCid(ownerHint) &&
        ownerHint != cs10) {
        x::runtime::LogW("DropPort",
                         "Drop.OwnerId self cache purge %u (==cs+0x10; ground=%u)", gDropSelfOwnerId,
                         ownerHint);
        gDropSelfOwnerId = 0;
    }
    if (gDropSelfOwnerId) return gDropSelfOwnerId;

    // 地上 OwnerId（含 ==cs 的角）反扫 WM → 钉 Drop 同槽
    if (PlausibleDropCid(ownerHint)) {
        void* wm = x::features::ports::world::PeekWorldManager();
        if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
        size_t off = 0;
        if (LooksLikeHeapPtr(wm) && ScanObjU32(wm, 0x10, 0x180, ownerHint, &off)) {
            x::features::ports::world::NoteDropOwnerWmFieldOff(off);
            gDropSelfOwnerId = ownerHint;
            static bool s_wmScanLogged = false;
            if (!s_wmScanLogged) {
                s_wmScanLogged = true;
                const uint32_t wm98 = static_cast<uint32_t>(ReadI32(wm, 0x98));
                x::runtime::LogI("DropPort",
                                 "Drop.OwnerId self=%u via WM+0x%zx scan (wm+0x98=%u cs+0x10=%u)",
                                 ownerHint, off, wm98, cs10);
            }
            return gDropSelfOwnerId;
        }
        if (LooksLikeHeapPtr(wm)) {
            void* box = ReadPtr(wm, 0x90);
            if (LooksLikeHeapPtr(box) && ScanObjU32(box, 0x10, 0x40, ownerHint, &off)) {
                gDropSelfOwnerId = ownerHint;
                static bool s_boxLogged = false;
                if (!s_boxLogged) {
                    s_boxLogged = true;
                    x::runtime::LogI("DropPort",
                                     "Drop.OwnerId self=%u via WM+0x90 box+0x%zx (cs+0x10=%u)",
                                     ownerHint, off, cs10);
                }
                return gDropSelfOwnerId;
            }
        }
    }

    const uint32_t dropCid = x::features::ports::world::GetDropOwnerCharacterId();
    // GetDropOwnerCharacterId 已排除 WM+0x98==CS；+0x114 允许 ==CS（本角合一）
    if (PlausibleDropCid(dropCid)) {
        gDropSelfOwnerId = dropCid;
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            x::runtime::LogI(
                "DropPort",
                "Drop.OwnerId self=%u via WM field/ByPetParity (cs+0x10=%u hint=%u pinOff=0x%zx)",
                dropCid, cs10, ownerHint, x::features::ports::world::PeekDropOwnerWmFieldOff());
        }
        return gDropSelfOwnerId;
    }

    // 地上 OwnerId 即 CS：信任为 Drop 归属（BIN 本角 195466）
    if (PlausibleDropCid(ownerHint) && cs10 && ownerHint == cs10) {
        gDropSelfOwnerId = ownerHint;
        static bool s_eqCsLogged = false;
        if (!s_eqCsLogged) {
            s_eqCsLogged = true;
            x::runtime::LogI("DropPort",
                             "Drop.OwnerId self=%u (==cs+0x10; ground OwnerId equals CS on this char)",
                             ownerHint);
        }
        return gDropSelfOwnerId;
    }

    static DWORD s_lastProbeMs = 0;
    const DWORD now = GetTickCount();
    if (!PlausibleDropCid(ownerHint) || (s_lastProbeMs && now - s_lastProbeMs < 2000u)) {
        static bool s_probe = false;
        if (!s_probe && PlausibleDropCid(ownerHint)) {
            s_probe = true;
            void* wm = x::features::ports::world::PeekWorldManager();
            if (!LooksLikeHeapPtr(wm)) wm = x::features::ports::world::GetWorldManager();
            const uint32_t wm98 = LooksLikeHeapPtr(wm) ? static_cast<uint32_t>(ReadI32(wm, 0x98)) : 0;
            void* box = LooksLikeHeapPtr(wm) ? ReadPtr(wm, 0x90) : nullptr;
            x::runtime::LogI("DropPort",
                             "Drop.OwnerId probe hint=%u cs=%u wm=%p +98=%u box=%p get=%u", ownerHint,
                             cs10, wm, wm98, box, dropCid);
        }
        return 0;
    }
    s_lastProbeMs = now;

    size_t off = 0;
    void* mu = gLocalUser ? gLocalUser : x::ui::player::LocalMyUser();
    if (ScanObjU32(mu, 0x10, 0x400, ownerHint, &off)) {
        gDropSelfOwnerId = ownerHint;
        if (!gRemoteDropOwnerOff) gRemoteDropOwnerOff = off;
        x::runtime::LogI("DropPort", "Drop.OwnerId self=%u @ User+0x%zx (cs+0x10=%u)", ownerHint,
                         off, cs10);
        return gDropSelfOwnerId;
    }
    void* cd = x::features::ports::world::GetCharacterData();
    if (ScanObjU32(cd, 0x10, 0x300, ownerHint, &off)) {
        gDropSelfOwnerId = ownerHint;
        x::runtime::LogI("DropPort",
                         "Drop.OwnerId self=%u @ CharacterData+0x%zx (cs+0x10=%u)", ownerHint, off,
                         cs10);
        return gDropSelfOwnerId;
    }
    return 0;
}

bool DropClientPickable(void* drop) {
    if (!LooksLikeHeapPtr(drop)) return false;
    const int ownerId = ReadI32(drop, kOffDropOwnerId);
    const int ownType = ReadDropOwnType(drop);

    RefreshRemoteDropOwnersIfDue();

    const uint32_t hint = ownerId > 0 ? static_cast<uint32_t>(ownerId) : 0;
    const uint32_t myId = LocalDropSelfOwnerId(hint);

    auto allowOwned = [&]() -> bool {
        if (ownerId == 0) return true;
        // 独图：无远程玩家，但可能有「别人走后残留、尚未变无主」的物。
        // 已认亲 → 只收自己；未认亲 → 放行（分不清己/残留，服端会拒非己）。
        if (gRemotePeekOk && gRemoteCountCached <= 0) {
            if (myId == 0) return true;
            if (hint == myId) return true;
            // 独图却对不上：缓存认亲错了（典型 CS 毒）。清掉后本拍放行，靠真吸重学。
            if (gDropSelfOwnerId == myId) {
                x::runtime::LogW("DropPort",
                                 "Drop.OwnerId self mismatch alone cache=%u drop=%u → purge", myId,
                                 hint);
                gDropSelfOwnerId = 0;
            }
            return true;
        }
        if (IsRemoteDropOwner(hint)) return false;
        // 多人兜底：已认亲则只收自己；顺带学远程 User 上的 OwnerId 槽
        if (myId != 0) {
            if (hint == myId) return true;
            MaybeLearnRemoteDropOwnerOff(hint);
            return false;
        }
        // 远程集合未命中且未认亲：放行（避免再堵吸）；服端仍会拒收非己
        return true;
    };

    if (ownType >= 0 && ownType <= 4) {
        if (ownType == 1) return false;                 // Party
        if (ownType == 2 || ownType == 3) return true;  // No / ExplosiveNoOwn
        if (ownType == 0 || ownType == 4) return allowOwned();
        return false;
    }

    return allowOwned();
}

// Drop.Pt1：CMS/TW 均为 System.Drawing.Point（int x/y），禁止当 float 读。
bool ReadDropPt(void* drop, float& x, float& y) {
    x = y = 0.f;
    if (!drop) return false;
    x = static_cast<float>(ReadI32(drop, kOffDropPt1));
    y = static_cast<float>(ReadI32(drop, kOffDropPt1 + 4));
    return true;
}

// 官方玩家拾取矩形比的是 PickPt@0x98（Point，Maple Y-down），不是 Pt1@0x20。
// 极早期（刚生成、还在抛物线上）可能仍是 (0,0)：回落 Pt1，宁可用旧点也别把整件漏掉。
bool ReadDropPickPt(void* drop, float& x, float& y) {
    x = y = 0.f;
    if (!drop) return false;
    const int px = ReadI32(drop, kOffDropPickPt);
    const int py = ReadI32(drop, kOffDropPickPt + 4);
    if (px != 0 || py != 0) {
        x = static_cast<float>(px);
        y = static_cast<float>(py);
        return true;
    }
    return ReadDropPt(drop, x, y);
}

void* FindClass(const char* name) {
    return x::runtime::il2cpp::FindClass("", name);
}

void* FindClassTypeObject(const char* className) {
    return x::runtime::il2cpp::FindClassTypeObject(className);
}

bool PlausibleInstanceOff(size_t off) {
    return off >= 0x10 && off < 0x1000;
}

bool PlausibleStaticOff(size_t off) {
    return off < 0x400;
}

bool FieldOffOrFb(void* klass, const char* fieldHash, size_t fb, size_t* out, bool staticOk) {
    *out = fb;
    if (!klass || !fieldHash) return false;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) return false;
    void* field = nullptr;
    __try {
        field = e.classGetFieldFromName(klass, fieldHash);
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
    if (staticOk) {
        if (!PlausibleStaticOff(off)) return false;
    } else if (!PlausibleInstanceOff(off)) {
        return false;
    }
    *out = off;
    return true;
}

constexpr int kFieldExpect = 21;
constexpr DWORD kFieldRetryMs = 3000;

// 人物直吸门禁字段（Pt1 / Pickable / Id / EndPara）是否来自元数据。
// 盒选读 Pt1、送包用角色坐标；PickPt 仅脚边路径使用，不纳入本门禁。
bool gCharGatesMeta = false;

void ReportFieldLamp() {
    char detail[48]{};
    snprintf(detail, sizeof(detail), "%s %d/%d w=%d", gOff.path, gOff.hits, kFieldExpect,
             gOff.writeSafe ? 1 : 0);
    using x::runtime::anchor_lamps::AnchorLampCode;
    const AnchorLampCode code = gOff.hits == kFieldExpect
                                    ? AnchorLampCode::Ok
                                    : (gOff.hits ? AnchorLampCode::Degraded : AnchorLampCode::Miss);
    x::runtime::anchor_lamps::Set("DropFields", code, detail);
}

// 未解析满就每 kFieldRetryMs 重试一次；日志只在 hits 变化时打，避免重试刷屏。
void EnsureFieldOffsets() {
    if (gOff.tried) return;
    const DWORD now = GetTickCount();
    if (gOff.nextTry && static_cast<int32_t>(now - gOff.nextTry) < 0) return;
    gOff.nextTry = now + kFieldRetryMs;
    if (!x::runtime::il2cpp::Ensure()) {
        if (gOff.loggedHits != -2) {
            gOff.loggedHits = -2;
            x::runtime::LogW("DropPort", "field offsets: bind miss — 写路径关闭，待重试");
        }
        ReportFieldLamp();
        return;
    }
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classGetFieldFromName || !e.fieldGetOffset) {
        if (gOff.loggedHits != -3) {
            gOff.loggedHits = -3;
            x::runtime::LogW("DropPort", "field offsets: exports miss — 写路径关闭，待重试");
        }
        ReportFieldLamp();
        return;
    }

    void* dropKlass = FindClass(kDropClass);
    void* poolKlass = FindClass(kDropPoolClass);
    void* petKlass = FindClass(kPetClass);
    void* userKlass = FindClass(kUserClass);
    void* vcoKlass = FindClass(kVecCtrlOwnerClass);
    void* vcKlass = FindClass(kVecCtrlClass);
    void* slotKlass = FindClass(kItemSlotPetClass);
    void* colKlass = FindClass(kCollisionCheckClass);
    void* wmKlass = x::runtime::il2cpp_shape::ResolveWorldManagerKlass();

    int hits = 0;
    auto hit = [&](bool ok) {
        if (ok) ++hits;
        return ok;
    };

    hit(FieldOffOrFb(userKlass, kHashFldApPet, kFbApPet, &gOff.apPet, false));
    hit(FieldOffOrFb(wmKlass, kHashFldWmMyUser, kFbWmMyUser, &gOff.wmMyUser, false));
    hit(FieldOffOrFb(poolKlass, kHashFldPoolDict, kFbPoolDict, &gOff.poolDict, false));
    hit(FieldOffOrFb(petKlass, kHashFldPetRc, kFbPetRc, &gOff.petRc, false));
    const bool okExcList = hit(FieldOffOrFb(petKlass, kHashFldPetExceptionList, kFbPetExceptionList,
                                            &gOff.petExceptionList, false));
    hit(FieldOffOrFb(vcoKlass, kHashFldVecCtrl, kFbVecCtrl, &gOff.vecCtrl, false));
    hit(FieldOffOrFb(vcoKlass, kHashFldFieldPos, kFbFieldPos, &gOff.fieldPos, false));
    hit(FieldOffOrFb(userKlass, kHashFldCurPos, kFbCurPos, &gOff.curPos, false));
    hit(FieldOffOrFb(vcKlass, kHashFldVcAp, kFbVcAp, &gOff.vcAp, false));
    const bool okId = hit(FieldOffOrFb(dropKlass, kHashFldDropId, kFbDropId, &gOff.dropId, false));
    // OwnerId：无独立 hash 时用 CMS 死钉 0x34（与 Id@0x30 相邻；归属预筛主路径）
    gOff.dropOwnerId = kFbDropOwnerId;
    // OwnType：CMS hash 在 TW 常误解析到 Id/SourceId（BIN ownType=8xxxxxx）。
    // ByPetParity：`mov ecx,[rdi+3Ch]` + seed 解出 cmp User=0 → 死钉 0x3C。
    (void)FieldOffOrFb(dropKlass, kHashFldDropOwnType, kFbDropOwnType, &gOff.dropOwnType, false);
    if (gOff.dropOwnType != kFbDropOwnType) {
        x::runtime::LogW("DropPort", "Drop.OwnType hashOff=0x%zx → pin ByPetParity 0x3C",
                         gOff.dropOwnType);
        gOff.dropOwnType = kFbDropOwnType;
    }
    const bool okOwn = true;
    hit(okOwn);
    // IsMoney：ByPet `movzx eax,byte [rdi+44h]`；fb=0x44（CMS 曾在 0x40）
    hit(FieldOffOrFb(dropKlass, kHashFldDropIsMoney, kFbDropIsMoney, &gOff.dropIsMoney, false));
    if (gOff.dropIsMoney != kFbDropIsMoney) {
        x::runtime::LogW("DropPort", "Drop.IsMoney hashOff=0x%zx → pin ByPetParity 0x44",
                         gOff.dropIsMoney);
        gOff.dropIsMoney = kFbDropIsMoney;
    }
    hit(FieldOffOrFb(dropKlass, kHashFldDropInfo, kFbDropInfo, &gOff.dropInfo, false));
    const bool okPt1 =
        hit(FieldOffOrFb(dropKlass, kHashFldDropPt1, kFbDropPt1, &gOff.dropPt1, false));
    const bool okEnd =
        hit(FieldOffOrFb(dropKlass, kHashFldDropEndPara, kFbDropEndPara, &gOff.dropEndPara, false));
    const bool okLast =
        hit(FieldOffOrFb(dropKlass, kHashFldDropLastTry, kFbDropLastTry, &gOff.dropLastTry, false));
    const bool okStamp = hit(
        FieldOffOrFb(dropKlass, kHashFldDropPickStamp, kFbDropPickStamp, &gOff.dropPickStamp, false));
    const bool okPickPt =
        hit(FieldOffOrFb(dropKlass, kHashFldDropPickPt, kFbDropPickPt, &gOff.dropPickPt, false));
    (void)okPickPt;  // 脚边路径用；人物直吸 meta 不依赖
    const bool okPickable = hit(
        FieldOffOrFb(dropKlass, kHashFldDropPickable, kFbDropPickable, &gOff.dropPickable, false));
    gCharGatesMeta = okPt1 && okPickable && okId && okEnd;
    hit(FieldOffOrFb(slotKlass, kHashFldItemSlotPetSkill, kFbItemSlotPetSkill, &gOff.itemSlotPetSkill,
                     false));
    hit(FieldOffOrFb(colKlass, kHashFldCollisionRcPet, kFbCollisionRcPet, &gOff.collisionRcPet, true));

    gOff.hits = hits;
    // Pt1 定位写哪些 drop、Id 是退避键，与 4 个写目标同等关键
    gOff.writeSafe = okPt1 && okId && okOwn && okEnd && okLast && okStamp && okExcList;
    gOff.path = hits == kFieldExpect ? "meta" : (hits ? "meta-partial" : "fallback");
    gOff.tried = hits == kFieldExpect;
    ReportFieldLamp();
    if (gOff.loggedHits == hits) return;
    gOff.loggedHits = hits;
    const char* fmt =
        "field offsets path=%s hits=%d/%d write=%d apPet=0x%zx poolDict=0x%zx pt1=0x%zx "
        "endPara=0x%zx vecCtrl=0x%zx vcAp=0x%zx curPos=0x%zx wmMy=0x%zx";
    if (gOff.tried) {
        x::runtime::LogI("DropPort", fmt, gOff.path, hits, kFieldExpect, gOff.writeSafe ? 1 : 0,
                         gOff.apPet, gOff.poolDict, gOff.dropPt1, gOff.dropEndPara, gOff.vecCtrl,
                         gOff.vcAp, gOff.curPos, gOff.wmMyUser);
    } else {
        // 未满：死钉可能已随客户端更新失效，写托管字段一律停手，等重试
        x::runtime::LogW("DropPort", fmt, gOff.path, hits, kFieldExpect, gOff.writeSafe ? 1 : 0,
                         gOff.apPet, gOff.poolDict, gOff.dropPt1, gOff.dropEndPara, gOff.vecCtrl,
                         gOff.vcAp, gOff.curPos, gOff.wmMyUser);
    }
}

MethodInfoHead* FindMethodByRva(void* klass, uint32_t rva) {
    if (!klass || !gClassGetMethods || !gGA) return nullptr;
    const void* want = AtRva<void*>(rva);
    void* iter = nullptr;
    __try {
        while (true) {
            void* mi = gClassGetMethods(klass, &iter);
            if (!mi) break;
            auto* head = reinterpret_cast<MethodInfoHead*>(mi);
            if (head->methodPointer == want || head->virtualMethodPointer == want) return head;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

// 走全仓 SSOT：hash → 明文 → RVA/kind，每档都过形状校验（旧的本地名字扫描不校验形状，已退役）
MethodInfoHead* ResolveMi(void* klass, uint32_t rva,
                          const x::runtime::il2cpp_method::MethodShape& shape,
                          const char* plain, const char* hash) {
    if (!klass) return nullptr;
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(klass, rva, shape, plain, hash);
    if (mr.method) {
        if (mr.path == x::runtime::il2cpp_method::ResolvePath::Kind) {
            x::runtime::LogI("DropPort", "ResolveMi kind hit rva=0x%X plain=%s", rva,
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

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    *outOrig = mi->methodPointer ? mi->methodPointer : mi->virtualMethodPointer;
    mi->methodPointer = hook;
    // 虚派发读 virtualMethodPointer；只换 methodPointer 时 ByPet 仍可能走原生。
    if (mi->virtualMethodPointer == *outOrig || mi->virtualMethodPointer == nullptr)
        mi->virtualMethodPointer = hook;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return true;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    void* cur = mi->methodPointer;
    mi->methodPointer = orig;
    if (mi->virtualMethodPointer == cur) mi->virtualMethodPointer = orig;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
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

bool PatchVtableMethodPtr(void** slot, void* hook, void** outOrig) {
    if (!slot || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) return false;
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

// 只认 VirtualInvokeData.method == 本 MI（unity_kbd：裸扫 methodPtr 会改到派发不读的格子）。
int PatchTryPickVtable(void* klass, MethodInfoHead* mi, void* hook) {
    if (!klass || !mi || !hook) return 0;
    int n = 0;
    constexpr size_t kLo = 0x80;
    constexpr size_t kHi = 0xC00;
    for (size_t off = kLo; off + 16 <= kHi; off += 8) {
        void* p0 = nullptr;
        void* p1 = nullptr;
        __try {
            p0 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off);
            p1 = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off + 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        (void)p0;
        if (p1 != mi) continue;
        if (p0 == hook) continue;
        void** slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(klass) + off);
        bool seen = false;
        for (int i = 0; i < gTryPickVtN; ++i) {
            if (gTryPickVtSlot[i] == slot) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        void* orig = nullptr;
        if (!PatchVtableMethodPtr(slot, hook, &orig)) continue;
        if (gTryPickVtN < kTryPickVtCap) {
            gTryPickVtSlot[gTryPickVtN] = slot;
            gTryPickVtOrig[gTryPickVtN] = orig;
            ++gTryPickVtN;
        }
        ++n;
    }
    return n;
}

void StampSkipBeforeNativePick(void* pet) {
    const SkipIds& skip = LiveSkip();
    if (skip.empty()) return;
    if (!LooksLikeHeapPtr(pet)) return;
    void* pool = gDropPool;
    if (!LooksLikeHeapPtr(pool)) return;
    (void)EnsureExceptionIdsIfNeeded(pet, skip);
    const int n = StampSkippedDropsNear(pool, 0.f, 0.f, 1e9f, 1e9f, skip, nullptr, nullptr, nullptr);
    if (n <= 0) return;
    static DWORD sLog = 0;
    const DWORD now = GetTickCount();
    if (!sLog || now - sLog >= 2000u) {
        sLog = now;
        x::runtime::LogI("droppool",
                         "skip-hold try-pick stamp=%d skipN=%d hits=%u (land-frame MI)", n,
                         (int)skip.size(), gTryPickHookHits.load(std::memory_order_relaxed));
    }
}

void HookPetTryPickUp(void* pet, const void* methodInfo) {
    thread_local int depth = 0;
    auto callOrig = [&]() {
        void* orig = gOrigTryPickUp;
        if (methodInfo) {
            for (int i = 0; i < gTryPickMiN; ++i) {
                if (gTryPickMi[i].mi == methodInfo && gTryPickMi[i].orig) {
                    orig = gTryPickMi[i].orig;
                    break;
                }
            }
        }
        auto* fn = reinterpret_cast<FnPetTryPickUp>(orig);
        if (fn) fn(pet, methodInfo);
    };
    if (depth > 0) {
        callOrig();
        return;
    }
    ++depth;
    StampSkipBeforeNativePick(pet);
    gTryPickHookHits.fetch_add(1, std::memory_order_relaxed);
    callOrig();
    --depth;
}

bool PatchOneTryPickMi(MethodInfoHead* mi, void* hook) {
    if (!mi || !hook) return false;
    for (int i = 0; i < gTryPickMiN; ++i) {
        if (gTryPickMi[i].mi == mi) return gTryPickMi[i].orig != nullptr;
    }
    void* orig = nullptr;
    if (!PatchMethodInfo(mi, hook, &orig) || !orig) return false;
    if (gTryPickMiN >= kTryPickMiCap) return true;
    gTryPickMi[gTryPickMiN].mi = mi;
    gTryPickMi[gTryPickMiN].orig = orig;
    ++gTryPickMiN;
    return true;
}

void EnsureTryPickProbe() {
    if (gTryPickProbeInstalled.load(std::memory_order_acquire)) return;
    if (!gGA || !gClassGetMethods) return;
    if (!gPetKlass) gPetKlass = FindClass(kPetClass);
    if (!gPetKlass) return;

    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kTry{0, TypeKind::Void, true, false, {}};
    if (!gMiTryPickUp) {
        gMiTryPickUp = ResolveMi(gPetKlass, kRvaPetTryPickUpDrop, kTry, "TryPickUpDrop",
                                 kHashPetTryPickUp);
    }

    MethodInfoHead* extra[4]{};
    int extraN = 0;
    auto pushMi = [&](MethodInfoHead* m) {
        if (!m) return;
        for (int i = 0; i < extraN; ++i) {
            if (extra[i] == m) return;
        }
        if (extraN < 4) extra[extraN++] = m;
    };
    pushMi(gMiTryPickUp);
    pushMi(FindMethodByRva(gPetKlass, kRvaPetTryPickStub));
    pushMi(FindMethodByRva(gPetKlass, kRvaPetTryPickBody));
    pushMi(FindMethodByRva(gPetKlass, kRvaPetTryPickUpDrop));

    auto* hook = reinterpret_cast<void*>(&HookPetTryPickUp);
    int patched = 0;
    for (int i = 0; i < extraN; ++i) {
        if (PatchOneTryPickMi(extra[i], hook)) ++patched;
    }
    if (gMiTryPickUp) {
        for (int i = 0; i < gTryPickMiN; ++i) {
            if (gTryPickMi[i].mi == gMiTryPickUp) {
                gOrigTryPickUp = gTryPickMi[i].orig;
                break;
            }
        }
    }
    if (!gOrigTryPickUp && gTryPickMiN > 0) gOrigTryPickUp = gTryPickMi[0].orig;

    int vt = 0;
    for (int i = 0; i < gTryPickMiN; ++i) {
        vt += PatchTryPickVtable(gPetKlass, gTryPickMi[i].mi, hook);
    }

    if (patched <= 0) return;
    gTryPickProbeInstalled.store(true, std::memory_order_release);
    uint32_t origRva = 0;
    if (gOrigTryPickUp && gGA) {
        const auto a = reinterpret_cast<uintptr_t>(gOrigTryPickUp);
        const auto b = reinterpret_cast<uintptr_t>(gGA);
        if (a >= b) origRva = static_cast<uint32_t>(a - b);
    }
    x::runtime::LogI("DropPort",
                     "TryPickProbe MI n=%d vt=%d origRva=0x%X (native pet tick; Send 仍是 E8)",
                     patched, vt, origRva);
}

bool __fastcall HookPetSend(void* self, uint64_t ptPacked, int dropId, uint32_t crc1, uint32_t crc2,
                            const void* methodInfo) {
    if (ShouldSwallowPickup(dropId)) {
        return false;
    }
    gPetSendHits.fetch_add(1, std::memory_order_relaxed);
    auto* fn = reinterpret_cast<FnPetSend>(gOrigPetSend);
    return fn ? fn(self, ptPacked, dropId, crc1, crc2, methodInfo) : false;
}

void __fastcall HookPoolSend(void* self, const void* ptIn, int dropId, uint32_t crc,
                             const void* methodInfo) {
    if (ShouldSwallowPickup(dropId)) {
        return;
    }
    gPoolSendHits.fetch_add(1, std::memory_order_relaxed);
    auto* fn = reinterpret_cast<FnPoolSend>(gOrigPoolSend);
    if (fn) fn(self, ptIn, dropId, crc, methodInfo);
}

void EnsureSendProbe() {
    if (!gSendProbeInstalled.load(std::memory_order_acquire)) {
        if (!gGA || !gClassGetMethods) {
            EnsureTryPickProbe();
            return;
        }
        if (!gPetKlass) gPetKlass = FindClass(kPetClass);
        if (!gDropPoolKlass) gDropPoolKlass = FindClass(kDropPoolClass);
        bool okPet = false;
        bool okPool = false;
        if (gPetKlass && !gMiPetSend) {
            using x::runtime::il2cpp_method::MethodShape;
            using x::runtime::il2cpp_method::TypeKind;
            // bool(Point,int,uint,uint) 唯一。
            constexpr MethodShape kPetSend{
                4, TypeKind::Bool, true, false,
                {TypeKind::Any, TypeKind::I32, TypeKind::U32, TypeKind::U32}};
            gMiPetSend = ResolveMi(gPetKlass, kRvaPetSendDropPickUp, kPetSend, "SendDropPickUpRequest",
                                   kHashPetSendDropPickUp);
            if (gMiPetSend &&
                PatchMethodInfo(gMiPetSend, reinterpret_cast<void*>(&HookPetSend), &gOrigPetSend)) {
                okPet = true;
            }
        } else if (gMiPetSend && gOrigPetSend) {
            okPet = true;
        }
        if (gDropPoolKlass && !gMiPoolSend) {
            using x::runtime::il2cpp_method::MethodShape;
            using x::runtime::il2cpp_method::TypeKind;
            // void(in Point,int,uint) 唯一。
            constexpr MethodShape kPoolSend{
                3, TypeKind::Void, true, false, {TypeKind::Any, TypeKind::I32, TypeKind::U32}};
            gMiPoolSend = ResolveMi(gDropPoolKlass, kRvaPoolSendDropPickUp, kPoolSend,
                                    "SendDropPickUpRequest", kHashPoolSendDropPickUp);
            if (gMiPoolSend &&
                PatchMethodInfo(gMiPoolSend, reinterpret_cast<void*>(&HookPoolSend),
                                &gOrigPoolSend)) {
                okPool = true;
            }
        } else if (gMiPoolSend && gOrigPoolSend) {
            okPool = true;
        }
        if (okPet || okPool) {
            gSendProbeInstalled.store(true, std::memory_order_release);
            x::runtime::LogI("DropPort",
                             "SendProbe MI pet=%d pool=%d (ByPet/Send 是 E8；原生拦捡走 TryPickProbe)",
                             okPet ? 1 : 0, okPool ? 1 : 0);
        }
    }
    EnsureTryPickProbe();
}

int ReadPoolDropCount(void* pool) {
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    const int count = ReadI32(dict, kOffDictCount);
    const int freeCount = ReadI32(dict, kOffDictFreeCount);
    if (count < 0 || count > 4096) return 0;
    const int live = count - freeCount;
    return live >= 0 ? live : count;
}

bool DropIdInPool(void* pool, int dropId) {
    return FindDropById(pool, dropId) != nullptr;
}

void* FindDropById(void* pool, int dropId) {
    if (!pool || dropId == 0) return nullptr;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return nullptr;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return nullptr;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return nullptr;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        if (ReadI32(drop, kOffDropId) == dropId) return drop;
    }
    return nullptr;
}

void* KlassStaticFields(void* klass) {
    if (!klass) return nullptr;
    if (gClassStaticData) {
        __try {
            void* p = gClassStaticData(klass);
            if (p) return p;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const size_t tryOffs[] = {0xB8, 0xB0, 0xC0, 0x5C, 0x90, 0xA8, 0xD0};
    for (size_t off : tryOffs) {
        void* p = ReadPtr(klass, off);
        if (LooksLikeHeapPtr(p)) return p;
    }
    return nullptr;
}

bool BindIl2Cpp() {
    if (gGA) {
        EnsureFieldOffsets();
        return true;
    }
    if (!x::runtime::il2cpp::Ensure()) return false;
    const auto& e = x::runtime::il2cpp::Get();
    gGA = e.ga;
    gFindAll = e.findAll;
    gClassGetMethods = e.classGetMethods;
    gClassStaticData = e.classStaticData;
    gClassParent = e.classParent;
    gRuntimeClassInit = e.runtimeClassInit;
    gCompGo = e.compGo;
    gObjName = e.objName;
    if (gGA) EnsureFieldOffsets();
    return gGA != nullptr;
}

bool ReadIl2CppString(void* str, char* out, size_t outCap) {
    out[0] = 0;
    if (!str || outCap < 2) return false;
    __try {
        const int len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 512) return false;
        const auto* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        size_t n = 0;
        for (int i = 0; i < len && n + 1 < outCap; ++i) {
            const wchar_t c = chars[i];
            if (c < 0x80) out[n++] = static_cast<char>(c);
            else if (c < 0x800 && n + 2 < outCap) {
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

bool ObjKlassIs(void* obj, void* expectKlass) {
    if (!obj || !expectKlass || !LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) == expectKlass;
}

// LocalUser / Component：+0x10 是 Unity m_CachedPtr（原生句柄），不能当托管堆指针校验。
bool LocalUserStillAlive(void* user) {
    // Worker-safe: 禁 GetGoName（managed → GC unknown thread）。
    if (!LooksLikeHeapPtr(user)) return false;
    __try {
        if (!ReadPtr(user, 0)) return false;
        return ReadPtr(user, 0x10) != nullptr;  // m_CachedPtr
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// DropPool 等纯托管 Singleton：只看 klass 指针。
bool ManagedAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    return ReadPtr(obj, 0) != nullptr;
}

// Pet 等 Unity 对象：m_CachedPtr@+0x10 非 0 即存活（原生句柄，勿 LooksLikeHeapPtr）。
bool UnityObjectAlive(void* obj) {
    if (!LooksLikeHeapPtr(obj)) return false;
    if (!ReadPtr(obj, 0)) return false;
    return ReadPtr(obj, 0x10) != nullptr;
}

// 解冻权：仅玩法就绪时清 freeze；大厅绑定 MyUser 不抢 auto_enter 的登录冻结。
void MaybeClearLoginFreeze() {
    if (!x::runtime::managed_main::IsLoginFrozen()) return;
    if (world::IsPlayReady()) x::runtime::managed_main::SetLoginFreeze(false);
}

bool ResolveLocalUser(DWORD now) {
    // 热路径：只看 Unity m_CachedPtr；名字校验放到真正 rebind。
    // 换图：WM.MyUser 指针变了立刻失效，不受 kRebindMs 保护。
    if (gLocalUser && now - gLastLuRebind < kRebindMs) {
        void* wm = world::PeekWorldManager();
        void* myUser = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
        if (LooksLikeHeapPtr(myUser) && myUser != gLocalUser) {
            gLocalUser = nullptr;
            ClearDropSelfOwnerCache();
            gLastLuRebind = 0;  // 强制 fall-through 立刻重绑
        } else if (!LooksLikeHeapPtr(myUser)) {
            // 换图空窗 MyUser 暂空：丢掉旧缓存，勿继续当活的用。
            gLocalUser = nullptr;
            ClearDropSelfOwnerCache();
            gLastLuRebind = 0;
        } else if (LooksLikeHeapPtr(gLocalUser) && ReadPtr(gLocalUser, 0) &&
                   ReadPtr(gLocalUser, 0x10)) {
            MaybeClearLoginFreeze();
            return true;
        } else {
            gLocalUser = nullptr;
            ClearDropSelfOwnerCache();
        }
    }
    if (gLastLuRebind && now - gLastLuRebind < kRebindMs && !gLocalUser) return false;
    gLastLuRebind = now;
    void* prevLu = gLocalUser;
    gLocalUser = nullptr;
    if (!BindIl2Cpp()) return false;

    void* wm = world::PeekWorldManager();
    if (!wm) wm = world::GetWorldManager();
    void* myUser = ReadPtr(wm, kOffWmMyUser);
    // 禁 GetGoName（worker）；WM.MyUser + m_CachedPtr 即权威。
    if (UnityObjectAlive(myUser)) {
        if (prevLu != myUser) {
            ClearDropSelfOwnerCache();
            x::runtime::LogI("DropPort", "LocalUser ACCEPT wm.MyUser=%p", myUser);
        }
        gLocalUser = myUser;
        MaybeClearLoginFreeze();
        return true;
    }

    // InterStage / 卸图：禁 LU FindAll（拖黑屏）；池侧 ResolveDropPool 已另有 PlayReady 闸。
    if (!world::IsPlayReady()) return false;

    if (!gLuType) {
        gLuType = x::runtime::il2cpp::ClassTypeObject(
            x::runtime::il2cpp_shape::ResolveUserLocalKlass());
    }
    if (!gLuType || !gFindAll) return false;

    void* arr = nullptr;
    __try {
        arr = x::runtime::managed_main::FindAll(gFindAll, gLuType, 2000);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uintptr_t n = ArrayLen(arr);
    for (uintptr_t i = 0; i < n && i < 64; ++i) {
        void* cand = ArrayAt(arr, i);
        if (!UnityObjectAlive(cand)) continue;
        if (prevLu != cand) {
            ClearDropSelfOwnerCache();
            x::runtime::LogI("DropPort", "LocalUser ACCEPT FindAll=%p", cand);
        }
        gLocalUser = cand;
        MaybeClearLoginFreeze();
        return true;
    }
    return false;
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

bool LooksLikeDropPool(void* cand) {
    if (!cand || !LooksLikeHeapPtr(cand)) return false;
    if (gDropPoolKlass && !ObjKlassIs(cand, gDropPoolKlass)) return false;
    void* dict = ReadPtr(cand, kOffPoolDict);
    // 允许 dict 尚未填充（刚进图）；但若有指针必须像堆。
    if (!dict) return true;
    return LooksLikeHeapPtr(dict);
}

int DropPoolScore(void* cand) {
    if (!LooksLikeDropPool(cand)) return -1;
    void* dict = ReadPtr(cand, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    const int count = ReadI32(dict, kOffDictCount);
    if (count < 0 || count > 4096) return 0;
    return count > 0 ? (1000 + (count > 500 ? 500 : count)) : 1;
}

void* ResolveDropPool(DWORD now) {
    // 离图/换图空窗：DropPool 被撕掉是常态。立刻作废缓存，且清负缓存，
    // 否则进图后仍吃 kRebindMs 盲区；也不把空窗刷成 W（捡物误报）。
    if (!world::IsPlayReady()) {
        gDropPool = nullptr;
        gLastPoolRebind = 0;
        return nullptr;
    }
    // 与 LocalUser 同口径：MyUser 暂空 = 换图中，池不可用；勿负缓存、勿 W。
    {
        void* wm = world::PeekWorldManager();
        void* myUser = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
        if (!LooksLikeHeapPtr(myUser)) {
            gDropPool = nullptr;
            gLastPoolRebind = 0;
            return nullptr;
        }
    }

    if (gDropPool && LooksLikeDropPool(gDropPool) && ManagedAlive(gDropPool) &&
        now - gLastPoolRebind < kRebindMs)
        return gDropPool;
    // 仅玩法就绪时的 miss 才短退避，避免 FindAll 空转；进图瞬间 gLast=0 可立刻重试。
    if (gLastPoolRebind && now - gLastPoolRebind < kRebindMs && !gDropPool) return nullptr;
    gLastPoolRebind = now;
    void* prevPool = gDropPool;
    gDropPool = nullptr;
    if (!gDropPoolKlass) gDropPoolKlass = FindClass(kDropPoolClass);
    if (!gDropPoolKlass) {
        x::runtime::LogWThrottled(21, 15000, "DropPort", "DropPool klass miss");
        return nullptr;
    }

    if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(gDropPoolKlass);

    void* staticsKlass = gDropPoolKlass;
    if (gClassParent) {
        void* parent = nullptr;
        __try {
            parent = gClassParent(gDropPoolKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (parent) {
            if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(parent);
            staticsKlass = parent;
        }
    }

    void* statics = KlassStaticFields(staticsKlass);
    if (!statics) statics = KlassStaticFields(gDropPoolKlass);

    void* best = nullptr;
    int bestScore = -1;
    if (statics) {
        // Singleton<T>.Lazy 通常在 parent statics +0；扫宽一点。
        for (size_t s = 0; s <= 0x40; s += sizeof(void*)) {
            void* lazy = ReadPtr(statics, s);
            void* cand = TryLazyValue(lazy);
            if (!cand) cand = lazy;
            if (LooksLikeHeapPtr(cand) && ObjKlassIs(cand, gDropPoolKlass)) {
                const int sc = DropPoolScore(cand);
                if (sc > bestScore) {
                    bestScore = sc;
                    best = cand;
                    if (sc >= 1000) break;
                }
            }
        }
    }

    // Fallback：FindAll(DropPool) — Lazy 未初始化或 static 槽扫空时兜底。
    if (!best) {
        void* typeObj = FindClassTypeObject(kDropPoolClass);
        if (typeObj && gFindAll) {
            void* arr = nullptr;
            __try {
                arr = x::runtime::managed_main::FindAll(gFindAll, typeObj, 2000);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                arr = nullptr;
            }
            const uintptr_t n = ArrayLen(arr);
            for (uintptr_t i = 0; i < n && i < 8; ++i) {
                void* cand = ArrayAt(arr, i);
                const int sc = DropPoolScore(cand);
                if (sc > bestScore) {
                    bestScore = sc;
                    best = cand;
                }
            }
            if (best) {
                x::runtime::LogI("DropPort", "DropPool FindAll hit %p score=%d n=%llu", best,
                                 bestScore, (unsigned long long)n);
            }
        }
    }

    if (best) {
        gDropPool = best;
        if (best != prevPool) {
            x::runtime::LogI("DropPort", "DropPool bind %p score=%d", gDropPool, bestScore);
        }
        return gDropPool;
    }
    // 玩法就绪仍扫不到才告警（真挂）；换图空窗已在入口 return。
    x::runtime::LogWThrottled(22, 15000, "DropPort", "DropPool resolve miss (statics=%p klass=%p)",
                              statics, gDropPoolKlass);
    return nullptr;
}

// Unity VecCtrl.Ap → Maple/Drop.Pt1 空间。
// BIN（0.1.17 upload b894df）：X 对齐，Y 恒近似取反（petY≈−dropY，56/56 翻 Y 后进盒）。
// Ap/Pos 为 Unity Y-up；Drop.Pt1 / CurPos 为枫谷 Y-down。
void ApToMaplePos(double ax, double ay, float& x, float& y) {
    x = static_cast<float>(ax);
    y = static_cast<float>(-ay);
}

// Pet : VecCtrlOwner — 门控/近距用 Maple 空间对齐 Drop.Pt1。
// 禁止优先 Pos@0x64；禁止读 CurPos@0x240（User 专属，Pet 无此字段）。
void ReadPetPos(void* pet, float& x, float& y) {
    x = y = 0.f;
    if (!pet) return;
    void* vc = ReadPtr(pet, kOffVecCtrl);
    if (LooksLikeHeapPtr(vc)) {
        const double ax = ReadF64(vc, kOffVcApX);
        const double ay = ReadF64(vc, kOffVcApY);
        if (std::fabs(ax) >= kMinPosAbs || std::fabs(ay) >= kMinPosAbs) {
            ApToMaplePos(ax, ay, x, y);
            return;
        }
    }
    // Pos@0x64 同属 Unity Vector2，兜底同样翻 Y
    const float ux = ReadF32(pet, kOffFieldPos);
    const float uy = ReadF32(pet, kOffFieldPos + 4);
    x = ux;
    y = -uy;
}

Rect4 ReadRect(void* base, size_t off) {
    Rect4 r{};
    r.x = ReadF32(base, off);
    r.y = ReadF32(base, off + 4);
    r.w = ReadF32(base, off + 8);
    r.h = ReadF32(base, off + 12);
    return r;
}

void* FirstActivePet() {
    if (!gLocalUser) return nullptr;
    void* arr = ReadPtr(gLocalUser, kOffApPet);
    if (!LooksLikeHeapPtr(arr)) return nullptr;
    const uintptr_t n = ArrayLen(arr);
    if (n == 0 || n > 8) return nullptr;
    for (uintptr_t i = 0; i < n; ++i) {
        void* pet = ArrayAt(arr, i);
        if (LooksLikeHeapPtr(pet) && UnityObjectAlive(pet)) return pet;
    }
    return nullptr;
}

uint16_t ReadPetSkill(void* pet) {
    if (!pet || !gGA) return 0;
    if (!gMiGetSkill && gPetKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kSk{0, TypeKind::Any, true, false, {}};
        gMiGetSkill = ResolveMi(gPetKlass, kRvaPetGetUpgradePetSkill, kSk, "GetUpgradePetSkill",
                                kHashPetGetUpgradeSkill);
    }
    auto fn = FnFromMi<FnPetGetSkill>(gMiGetSkill, kRvaPetGetUpgradePetSkill);
    if (!fn) return 0;
    uint16_t skill = 0;
    __try {
        skill = fn(pet, gMiGetSkill);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        skill = 0;
    }
    return skill;
}

// ByPet 真源：GetItemSlot(pet) → ItemSlotPet.usPetSkill@+0x3C（勿再读 Pet+0x428）
uint16_t ReadPetSkillSlot(void* pet) {
    if (!pet || !gGA) return 0;
    if (!gMiGetItemSlot && gPetKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kSlot{0, TypeKind::Any, true, false, {}};
        gMiGetItemSlot =
            ResolveMi(gPetKlass, kRvaPetGetItemSlot, kSlot, "GetItemSlot", kHashPetGetItemSlot);
    }
    auto fn = FnFromMi<FnPetGetItemSlot>(gMiGetItemSlot, kRvaPetGetItemSlot);
    if (!fn) return 0;
    void* slot = nullptr;
    __try {
        slot = fn(pet, gMiGetItemSlot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slot = nullptr;
    }
    if (!LooksLikeHeapPtr(slot)) return 0;
    return ReadU16(slot, kOffItemSlotPetSkill);
}

// 只读：probe 对照原生托管矩形；ByPet Contains 不读这里。
Rect4 ReadCollisionRcPet() {
    Rect4 empty{};
    if (!gCollisionKlass) gCollisionKlass = FindClass(kCollisionCheckClass);
    if (!gCollisionKlass) return empty;
    if (gRuntimeClassInit) x::runtime::il2cpp::RuntimeClassInit(gCollisionKlass);
    void* statics = KlassStaticFields(gCollisionKlass);
    if (!LooksLikeHeapPtr(statics)) return empty;
    return ReadRect(statics, kOffCollisionRcPet);
}

// ByPet 用 .rdata 常量组 Contains 矩形；扩 _rcPet/CollisionCheck 对这条链无效。
struct ByPetRectBackup {
    int32_t offX = 25;
    int32_t offY = 10;
    float w = 50.f;
    float h = 60.f;
    bool ok = false;
    DWORD oldProtect = 0;
    uint8_t* base = nullptr;
};

bool ReadRectPackFields(const uint8_t* p, int32_t& offX, int32_t& offY, float& w, float& h) {
    if (!p) return false;
    __try {
        offX = *reinterpret_cast<const int32_t*>(p + 0);
        offY = *reinterpret_cast<const int32_t*>(p + 4);
        w = *reinterpret_cast<const float*>(p + 0x10);
        h = *reinterpret_cast<const float*>(p + 0x14);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool LooksLikeNativeRectPack(const uint8_t* p) {
    int32_t ox = 0, oy = 0;
    float w = 0.f, h = 0.f;
    if (!ReadRectPackFields(p, ox, oy, w, h)) return false;
    return ox == kNativeRectOffX && oy == kNativeRectOffY && w == kNativeRectW && h == kNativeRectH;
}

bool LooksLikeRectPackCandidate(const uint8_t* p) {
    // 原生包，或合理真空尺寸（避免误咬随机 .rdata）
    if (LooksLikeNativeRectPack(p)) return true;
    int32_t ox = 0, oy = 0;
    float w = 0.f, h = 0.f;
    if (!ReadRectPackFields(p, ox, oy, w, h)) return false;
    if (!(w >= 1.f && w <= 20000.f && h >= 1.f && h <= 20000.f)) return false;
    // off 通常 ≈ 半宽/半高（原生 25/10；扩盒后 W/2,H/2）
    const int32_t halfW = static_cast<int32_t>(w * 0.5f);
    const int32_t halfH = static_cast<int32_t>(h * 0.5f);
    return (ox == halfW || ox == kNativeRectOffX) && (oy == halfH || oy == kNativeRectOffY);
}

bool PtrInGaImage(const void* p) {
    if (!gGA || !p) return false;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), gGA, &mi, sizeof(mi))) return false;
    const auto b = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
    const auto a = reinterpret_cast<uintptr_t>(p);
    return a >= b && a + 0x18 <= b + mi.SizeOfImage;
}

// x64 RIP-relative：ModRM.mod=00 && r/m=101 → [rip+disp32]；目标 = modrm+5+disp
uint8_t* TryRipRelTarget(const uint8_t* modrm) {
    if (!modrm || (modrm[0] & 0xC7) != 0x05) return nullptr;
    int32_t disp = 0;
    __try {
        disp = *reinterpret_cast<const int32_t*>(modrm + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    auto* tgt = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(modrm) + 5u +
                                          static_cast<intptr_t>(disp));
    if (!PtrInGaImage(tgt)) return nullptr;
    return tgt;
}

uint8_t* ScanFnForRectPack(const uint8_t* fn, size_t maxScan) {
    if (!fn || !maxScan) return nullptr;
    uint8_t* nativeHit = nullptr;
    uint8_t* softHit = nullptr;
    int softHits = 0;
    __try {
        for (size_t i = 0; i + 5 < maxScan; ++i) {
            uint8_t* tgt = TryRipRelTarget(fn + i);
            if (!tgt) continue;
            if (LooksLikeNativeRectPack(tgt)) {
                nativeHit = tgt;
                break;
            }
            if (LooksLikeRectPackCandidate(tgt)) {
                if (!softHit || softHit == tgt) {
                    softHit = tgt;
                    if (softHits == 0) softHits = 1;
                } else {
                    softHits = 2;  // 多候选 → 不用软命中
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nativeHit ? nativeHit : (softHits == 1 ? softHit : nullptr);
    }
    if (nativeHit) return nativeHit;
    return softHits == 1 ? softHit : nullptr;
}

uint8_t* ScanGaImageForNativeRectPack() {
    if (!gGA) return nullptr;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), gGA, &mi, sizeof(mi))) return nullptr;
    auto* base = reinterpret_cast<uint8_t*>(mi.lpBaseOfDll);
    const size_t size = mi.SizeOfImage;
    if (!base || size < 0x20) return nullptr;
    static const uint8_t kPat[] = {
        0x19, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x48, 0x42, 0x00, 0x00, 0x70, 0x42};
    uint8_t* hit = nullptr;
    int hits = 0;
    uint8_t* p = base;
    const uint8_t* end = base + size;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) break;
        auto* reg = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
        const size_t regSize = mbi.RegionSize;
        const uint8_t* next = reg + regSize;
        const bool readable =
            mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)) != 0 &&
            (mbi.Protect & PAGE_GUARD) == 0;
        if (readable && regSize >= sizeof(kPat)) {
            const size_t lim = regSize - sizeof(kPat) + 1;
            __try {
                for (size_t i = 0; i < lim; ++i) {
                    if (memcmp(reg + i, kPat, sizeof(kPat)) != 0) continue;
                    // 只认落在本模块映像内的命中
                    if (reg + i < base || reg + i >= end) continue;
                    ++hits;
                    hit = reg + i;
                    if (hits > 1) return nullptr;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        p = const_cast<uint8_t*>(next > p ? next : p + 0x1000);
    }
    return hits == 1 ? hit : nullptr;
}

void ReportPetRectLamp() {
    if (gByPetRectPack) {
        char detail[48]{};
        snprintf(detail, sizeof(detail), "%s", gByPetRectVia ? gByPetRectVia : "ok");
        x::runtime::anchor_lamps::Set("PetRect", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                     detail);
    } else {
        x::runtime::anchor_lamps::Set("PetRect", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                     "MISS");
    }
}

uint8_t* ResolveByPetRectPack(bool force = false) {
    if (gByPetRectPack && !force) return gByPetRectPack;
    gByPetRectPack = nullptr;
    gByPetRectVia = nullptr;
    if (!BindIl2Cpp()) {
        ReportPetRectLamp();
        return nullptr;
    }

    // 1) ByPet 方法体 RIP 相对寻址（换版 RVA 漂时靠 MI/哈希拿 fn）
    if (!gDropPoolKlass) gDropPoolKlass = FindClass(kDropPoolClass);
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    // TryPickUpDropByPet：TypeSignature viiiii → arity≈3；哈希/RVA 钉死
    constexpr MethodShape kByPet{3, TypeKind::Void, false, false,
                                 {TypeKind::Any, TypeKind::Any, TypeKind::Any}};
    MethodInfoHead* miByPet = nullptr;
    if (gDropPoolKlass) {
        miByPet = ResolveMi(gDropPoolKlass, kRvaDropTryPickUpDropByPet, kByPet,
                            "TryPickUpDropByPet", kHashDropTryPickUpByPet);
    }
    const uint8_t* fn = nullptr;
    if (miByPet && miByPet->methodPointer)
        fn = reinterpret_cast<const uint8_t*>(miByPet->methodPointer);
    if (!fn) fn = AtRva<const uint8_t*>(kRvaDropTryPickUpDropByPet);
    if (fn) {
        if (uint8_t* hit = ScanFnForRectPack(fn, kByPetScanMax)) {
            gByPetRectPack = hit;
            gByPetRectVia = "ByPet xref";
        }
    }

    // 2) GA 映像唯一原生特征
    if (!gByPetRectPack) {
        if (uint8_t* hit = ScanGaImageForNativeRectPack()) {
            gByPetRectPack = hit;
            gByPetRectVia = "pattern";
        }
    }

    // 3) 末级：本版死钉 RVA
    if (!gByPetRectPack) {
        uint8_t* fb = AtRva<uint8_t*>(kRvaByPetRectPackFallback);
        if (fb && LooksLikeRectPackCandidate(fb)) {
            gByPetRectPack = fb;
            gByPetRectVia = "RVA fallback";
        }
    }

    if (gByPetRectPack) {
        const uintptr_t rva =
            reinterpret_cast<uintptr_t>(gByPetRectPack) - reinterpret_cast<uintptr_t>(gGA);
        x::runtime::LogI("DropPort", "ByPetRectPack bind ok via=%s ptr=%p rva=0x%llX",
                         gByPetRectVia ? gByPetRectVia : "?", gByPetRectPack,
                         (unsigned long long)rva);
    } else {
        x::runtime::LogW("DropPort", "ByPetRectPack resolve MISS (xref/pattern/RVA)");
    }
    ReportPetRectLamp();
    return gByPetRectPack;
}

bool PatchByPetRectPack(float vacuumW, float vacuumH, ByPetRectBackup& bak) {
    bak = {};
    if (gRectHold.active) return false;
    if (!gGA || vacuumW < 1.f || vacuumH < 1.f) return false;
    bak.base = ResolveByPetRectPack(false);
    if (!bak.base) return false;
    __try {
        bak.offX = *reinterpret_cast<int32_t*>(bak.base + 0);
        bak.offY = *reinterpret_cast<int32_t*>(bak.base + 4);
        bak.w = *reinterpret_cast<float*>(bak.base + 0x10);
        bak.h = *reinterpret_cast<float*>(bak.base + 0x14);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!VirtualProtect(bak.base, 0x20, PAGE_READWRITE, &bak.oldProtect)) return false;
    const int32_t offX = static_cast<int32_t>(vacuumW * 0.5f);
    const int32_t offY = static_cast<int32_t>(vacuumH * 0.5f);
    *reinterpret_cast<int32_t*>(bak.base + 0) = offX;
    *reinterpret_cast<int32_t*>(bak.base + 4) = offY;
    *reinterpret_cast<float*>(bak.base + 0x10) = vacuumW;
    *reinterpret_cast<float*>(bak.base + 0x14) = vacuumH;
    bak.ok = true;
    return true;
}

void RestoreByPetRectPack(ByPetRectBackup& bak) {
    if (gRectHold.active) {
        bak.ok = false;
        bak.base = nullptr;
        return;
    }
    if (!bak.ok || !bak.base) return;
    __try {
        *reinterpret_cast<int32_t*>(bak.base + 0) = bak.offX;
        *reinterpret_cast<int32_t*>(bak.base + 4) = bak.offY;
        *reinterpret_cast<float*>(bak.base + 0x10) = bak.w;
        *reinterpret_cast<float*>(bak.base + 0x14) = bak.h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    DWORD tmp = 0;
    VirtualProtect(bak.base, 0x20, bak.oldProtect, &tmp);
    bak.ok = false;
}

void ReleaseByPetRectPackImpl();

bool WriteRectPack(uint8_t* p, int32_t ox, int32_t oy, float w, float h) {
    if (!p) return false;
    __try {
        *reinterpret_cast<int32_t*>(p + 0) = ox;
        *reinterpret_cast<int32_t*>(p + 4) = oy;
        *reinterpret_cast<float*>(p + 0x10) = w;
        *reinterpret_cast<float*>(p + 0x14) = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool HoldByPetRectPackImpl(float vacuumW, float vacuumH) {
    if (vacuumW < 1.f || vacuumH < 1.f) return false;
    if (vacuumW < 50.f) vacuumW = 50.f;
    if (vacuumH < 50.f) vacuumH = 50.f;
    if (vacuumW > 20000.f) vacuumW = 20000.f;
    if (vacuumH > 20000.f) vacuumH = 20000.f;
    uint8_t* base = ResolveByPetRectPack(false);
    if (!base) return false;
    const int32_t offX = static_cast<int32_t>(vacuumW * 0.5f);
    const int32_t offY = static_cast<int32_t>(vacuumH * 0.5f);
    if (gRectHold.active && gRectHold.base == base && gRectHold.w == vacuumW &&
        gRectHold.h == vacuumH) {
        return true;
    }
    if (!gRectHold.active) {
        DWORD oldProtect = 0;
        if (!VirtualProtect(base, 0x20, PAGE_READWRITE, &oldProtect)) return false;
        gRectHold.base = base;
        gRectHold.oldProtect = oldProtect;
        gRectHold.active = true;
    } else if (gRectHold.base != base) {
        ReleaseByPetRectPackImpl();
        DWORD oldProtect = 0;
        if (!VirtualProtect(base, 0x20, PAGE_READWRITE, &oldProtect)) return false;
        gRectHold.base = base;
        gRectHold.oldProtect = oldProtect;
        gRectHold.active = true;
    }
    if (!WriteRectPack(base, offX, offY, vacuumW, vacuumH)) return false;
    gRectHold.w = vacuumW;
    gRectHold.h = vacuumH;
    x::runtime::LogI("DropPort", "native-vac hold %.0fx%.0f via=%s", vacuumW, vacuumH,
                     gByPetRectVia ? gByPetRectVia : "?");
    return true;
}

void ReleaseByPetRectPackImpl() {
    if (!gRectHold.active || !gRectHold.base) {
        gRectHold = {};
        return;
    }
    (void)WriteRectPack(gRectHold.base, kNativeRectOffX, kNativeRectOffY, kNativeRectW,
                        kNativeRectH);
    DWORD tmp = 0;
    VirtualProtect(gRectHold.base, 0x20, gRectHold.oldProtect, &tmp);
    x::runtime::LogI("DropPort", "native-vac release (restore 50x60)");
    gRectHold = {};
}

// 宠吸拍前门控（IDA ByPet 实锤）：
// - EndParabolicMotion 必须 == 3 才继续；绝不能写 0（重置抛物线 → 全体飞落）
// - +0x88 为 Send 盖戳冷却（对照 PickUpInterval=3000）
// - LastTry / 异常 OwnType 仍清
// - 黑名单 drop 一律不碰（否则每拍清 LastTry + 写 EndPara=3 → ByPet 空吸动画「打转」）
bool DropMatchesSkip(void* drop, const SkipIds& skip);

// 只清一件：人物直吸每拍只送 1 件，全盒 Clear 上百次写会占死 MainPump，打怪/瞬移饿死。
// EndPara 纪律（upload E226 / 065ed0 / BIN 2026-08-09 petloot）：
// - Ready(3)：只清 PickStamp / LastTry / 异常 OwnType，绝不改 EndPara
// - 抛物中（!=3 且非 SkipHold）：默认一律不写——清冷却也会搅动画态，落地打转
// - SkipHold(4)：默认不碰（黑名单长期戳）；force=true 才允许升回 3（遗留路径）
// - forceEndParaReady=true：!=0 写成 3（禁写 0）；宠吸禁止 force
bool ClearPickupGatesOne(void* drop, bool forceEndParaReady = false) {
    if (!LooksLikeHeapPtr(drop) || !DropWritesAllowed()) return false;
    const int endp = ReadI32(drop, kOffDropEndPara);
    if (endp != kEndParaReady) {
        if (!forceEndParaReady) return false;  // 抛物中 / SkipHold：默认零写
        if (endp == 0) return false;          // 禁写 0（重置抛物飞落）
        WriteI32(drop, kOffDropEndPara, kEndParaReady);
        // force 路径仍继续清冷却，便于遗留调用方一次到位
    }
    bool touched = (endp != kEndParaReady);  // force 已写 EndPara
    const int own = ReadDropOwnType(drop);
    const int last = ReadI32(drop, kOffDropLastTry);
    const int stamp = ReadI32(drop, kOffDropPickStamp);
    if (stamp != 0) {
        WriteI32(drop, kOffDropPickStamp, 0);
        touched = true;
    }
    // 退避戳 INT_MAX 留给 StampStalled，别在这里清掉
    if (last != 0 && last != kLastTrySkipStamp) {
        WriteI32(drop, kOffDropLastTry, 0);
        touched = true;
    }
    // OwnType 仅允许写合法枚举；未解析（-1）绝不写 No=2
    (void)own;
    return touched;
}

// 盒内有限清闸：宠吸走官方 TryPickUpDrop，无法预知「下一件」；全盒写会占死 MainPump。
// maxClear>0 时最多清这么多件（建议 ≈8，对齐 ByPet 一拍吞吐）；<=0 表示不限制。
// 黑名单/退避 LastTry=INT_MAX 不碰；不可捡的跳过。写路径复用 ClearPickupGatesOne。
int ClearPickupGatesNear(void* pool, float cx, float cy, float halfW, float halfH,
                         int* outSampleOwn, int* outSampleLast, int* outSampleEnd,
                         const SkipIds* skip, int maxClear = 0) {
    if (outSampleOwn) *outSampleOwn = -1;
    if (outSampleLast) *outSampleLast = 0;
    if (outSampleEnd) *outSampleEnd = 0;
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int cleared = 0;
    bool sampled = false;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        if (maxClear > 0 && cleared >= maxClear) break;
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        const int hash = ReadI32(entry, kOffEntryHash);
        if (hash < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        if (skip && !skip->empty() && DropMatchesSkip(drop, *skip)) continue;
        if (ReadU8(drop, kOffDropPickable) == 0) continue;
        if (ReadI32(drop, kOffDropLastTry) == kLastTrySkipStamp) continue;

        if (!sampled) {
            sampled = true;
            if (outSampleOwn) *outSampleOwn = ReadDropOwnType(drop);
            if (outSampleLast) *outSampleLast = ReadI32(drop, kOffDropLastTry);
            if (outSampleEnd) *outSampleEnd = ReadI32(drop, kOffDropEndPara);
        }
        if (ClearPickupGatesOne(drop, /*forceEndParaReady=*/false)) ++cleared;
    }
    return cleared;
}

int CountDropsNear(void* pool, float cx, float cy, float halfW, float halfH, int* outTotal,
                   int* outNearMoney = nullptr, int* outNearItem = nullptr,
                   int* outSampleIsMoney = nullptr, int* outSampleInfo = nullptr) {
    if (outTotal) *outTotal = 0;
    if (outNearMoney) *outNearMoney = 0;
    if (outNearItem) *outNearItem = 0;
    if (outSampleIsMoney) *outSampleIsMoney = -1;
    if (outSampleInfo) *outSampleInfo = 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int total = 0, nearN = 0, nearMoney = 0, nearItem = 0;
    bool haveMoneySample = false;
    bool haveAnySample = false;
    int sampleMoneyFlag = -1;
    int sampleInfo = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        const int hash = ReadI32(entry, kOffEntryHash);
        if (hash < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        ++total;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        ++nearN;
        const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
        const int info = ReadI32(drop, kOffDropInfo);
        if (money) {
            ++nearMoney;
            if (!haveMoneySample) {
                haveMoneySample = true;
                haveAnySample = true;
                sampleMoneyFlag = 1;
                sampleInfo = info;
            }
        } else {
            ++nearItem;
            if (!haveAnySample) {
                haveAnySample = true;
                sampleMoneyFlag = 0;
                sampleInfo = info;
            }
        }
    }
    if (outTotal) *outTotal = total;
    if (outNearMoney) *outNearMoney = nearMoney;
    if (outNearItem) *outNearItem = nearItem;
    if (outSampleIsMoney) *outSampleIsMoney = sampleMoneyFlag;
    if (outSampleInfo) *outSampleInfo = sampleInfo;
    return nearN;
}

// 拍前 ClearPickupGates 已清 LastTry/PickStamp；拍后非 0 = ByPet 本拍碰过（多为 Send）
// outStallIds/outStallN：本拍被 ByPet 盖戳但池未掉的 dropId（交给退避黑名单登记）
int CountPostSendTouchesNear(void* pool, float cx, float cy, float halfW, float halfH,
                             int* outTouchMoney, int* outStallIds, int stallCap, int* outStallN) {
    if (outTouchMoney) *outTouchMoney = 0;
    if (outStallN) *outStallN = 0;
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int touch = 0, touchMoney = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        const int last = ReadI32(drop, kOffDropLastTry);
        const int stamp = ReadI32(drop, kOffDropPickStamp);
        if (last == 0 && stamp == 0) continue;
        if (last == kLastTrySkipStamp) continue;  // 我们自己的黑名单/退避戳，不是 ByPet 提交
        ++touch;
        if (ReadU8(drop, kOffDropIsMoney) != 0) ++touchMoney;
        if (outStallIds && outStallN && *outStallN < stallCap) {
            const int did = ReadI32(drop, kOffDropId);
            if (did != 0) outStallIds[(*outStallN)++] = did;
        }
    }
    if (outTouchMoney) *outTouchMoney = touchMoney;
    return touch;
}

// sentSame 但 CountPostSendTouches 抓不到官方戳（stallN=0）时：仍要登记队头，否则
// gStall 空 → 下拍 stallHeld>0&stamp=0 错觉/或根本无 held → ByPet 反复撞同一拒收件，
// 金币被挡（BIN itemWhileMoney / reject_backoff）。
// 策略：盒内有钱则只退避 Ready 非钱；否则退避最近 Ready（仍跳过已 Stall / 黑名单）。
int AddStallFallbackHeadsNear(void* pool, float cx, float cy, float halfW, float halfH, DWORD now,
                              const SkipIds* skip, int cap) {
    if (!pool || gStallOff || cap <= 0) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    bool haveMoney = false;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        if (ReadU8(drop, kOffDropIsMoney) != 0) {
            haveMoney = true;
            break;
        }
    }

    int added = 0;
    int bestId = 0;
    float bestD2 = 0.f;
    bool haveBest = false;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        if (skip && !skip->empty() && DropMatchesSkip(drop, *skip)) continue;
        if (!DropClientPickable(drop)) continue;
        if (ReadU8(drop, kOffDropPickable) == 0) continue;
        if (ReadI32(drop, kOffDropEndPara) != kEndParaReady) continue;
        const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
        if (haveMoney && money) continue;  // 有钱时只挡道具队头
        const int id = ReadI32(drop, kOffDropId);
        if (id == 0 || StallActive(id, now)) continue;
        const float dx = px - cx;
        const float dy = py - cy;
        const float d2 = dx * dx + dy * dy;
        if (!haveBest || d2 < bestD2) {
            haveBest = true;
            bestD2 = d2;
            bestId = id;
        }
        if (haveMoney) {
            // 有钱：把盒内所有 Ready 非钱都登退避（盖戳后 ByPet 才能轮到钱）
            AddStall(id, now);
            ++added;
            if (added >= cap) return added;
        }
    }
    if (!haveMoney && haveBest && bestId != 0) {
        AddStall(bestId, now);
        ++added;
    }
    if (added > 0) {
        static DWORD s_lastFb = 0;
        if (!s_lastFb || now - s_lastFb > 3000u) {
            s_lastFb = now;
            x::runtime::LogI("droppool",
                             "stall fallback heads=%d haveMoney=%d (sentSame touch=0)", added,
                             haveMoney ? 1 : 0);
        }
    }
    return added;
}

bool EnsureExceptionIds(void* pet, const SkipIds& skip) {
    if (!pet || skip.empty()) return true;
    // 偏移没解析满时别往「可能不是 ExceptionList」的指针里追加 id（返回 false → 下次仍会重试）
    if (!DropWritesAllowed()) return false;
    void* list = ReadPtr(pet, kOffPetExceptionList);
    if (!LooksLikeHeapPtr(list)) return false;
    void* items = ReadPtr(list, kOffListItems);
    int size = ReadI32(list, kOffListSize);
    if (!LooksLikeHeapPtr(items) || size < 0 || size > 512) return false;
    const uintptr_t cap = ArrayLen(items);
    if (cap == 0 || cap > 1024) return false;

    // 已有表 → set，避免宽词三千次线性扫表（读写走 ReadI32/WriteI32，勿在本函数内 __try）
    std::unordered_set<int> have;
    have.reserve(static_cast<size_t>(size) + 16);
    for (int i = 0; i < size; ++i) {
        const int v = ReadI32(items, 0x20 + (uintptr_t)i * 4);
        if (v > 0) have.insert(v);
    }

    // 托管 ExceptionList 容量很小；主挡捡靠 LastTry+EndPara。此处只尽量补满 cap。
    for (const int id : skip.ids) {
        if (id <= 0) continue;
        if (have.find(id) != have.end()) continue;
        if ((uintptr_t)size >= cap) break;
        WriteI32(items, 0x20 + (uintptr_t)size * 4, id);
        if (ReadI32(items, 0x20 + (uintptr_t)size * 4) != id) return false;
        ++size;
        WriteI32(list, kOffListSize, size);
        have.insert(id);
    }
    return true;
}

// 仅在 skip 集合或宠指针变化时同步 ExceptionList（宽词每拍全量扫会卡主线程）
size_t HashSkipIds(const SkipIds& skip) {
    size_t h = skip.size();
    for (const int id : skip.ids) {
        h ^= static_cast<size_t>(id) + 0x9e3779b9u + (h << 6) + (h >> 2);
    }
    return h;
}

bool EnsureExceptionIdsIfNeeded(void* pet, const SkipIds& skip) {
    if (!pet || skip.empty()) return true;
    static void* s_lastPet = nullptr;
    static size_t s_lastHash = 0;
    const size_t h = HashSkipIds(skip);
    if (pet == s_lastPet && h == s_lastHash) return true;
    const bool ok = EnsureExceptionIds(pet, skip);
    if (ok) {
        s_lastPet = pet;
        s_lastHash = h;
    }
    return ok;
}

int StampSkippedDropsNear(void* pool, float cx, float cy, float halfW, float halfH,
                          const SkipIds& skip, int* outNear, int* outWant, int* outTotal);
int StampStalledDropsNear(void* pool, float cx, float cy, float halfW, float halfH, DWORD now,
                          const SkipIds* skip);
int RestoreStalledStampsNear(void* pool, float cx, float cy, float halfW, float halfH, DWORD now,
                             const SkipIds* skip);

// 宠吸近盒一次扫池：Count + 清闸(budget) + Stall 盖戳 + Skip 盖戳（语义对齐原多趟顺序）。
struct PetVacNearPass {
    int total = 0;
    int nearN = 0;
    int nearMoney = 0;
    int nearItem = 0;
    int readyNear = 0;  // EndPara==Ready 且可碰（非黑名单/非退避戳）
    int flyNear = 0;    // 盒内仍抛物中（EndPara!=Ready；已跳过黑名单）
    int sampleIsMoney = -1;
    int sampleInfo = 0;
    int sampleOwn = -1;
    int sampleOwnRaw = 0;
    int sampleOwnerId = 0;
    int sampleLast = 0;
    int sampleEnd = 0;
    int gatesCleared = 0;
    int stallStamped = 0;
    int skipStamped = 0;
    int ownSkipped = 0;   // 盒内非己/非无主（未进 readyNear）
    int ownStamped = 0;   // 为挡 ByPet 临时盖 LastTry 的件数（拍末还原）
    // 可吸盒内 OwnerId>0 的一致值；mixed 则本拍不拿来认亲（防多人图误学别人）
    int learnOwnerId = 0;
    bool learnOwnerMixed = false;
};

PetVacNearPass PreparePetVacNearPass(void* pool, float cx, float cy, float halfW, float halfH,
                                     DWORD now, const SkipIds* skip, int maxClear) {
    PetVacNearPass out{};
    gOwnSkipStamps.clear();
    if (!pool) return out;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return out;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return out;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return out;

    const bool writesOk = DropWritesAllowed();
    const bool doStall = writesOk && !gStallOff && !gStall.empty();
    const bool haveSkip = skip && !skip->empty();
    bool haveMoneySample = false;
    bool haveAnySample = false;
    bool gateSampled = false;

    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        ++out.total;

        float dpx = 0.f, dpy = 0.f;
        if (!ReadDropPt(drop, dpx, dpy)) continue;
        if (std::fabs(dpx - cx) > halfW || std::fabs(dpy - cy) > halfH) continue;
        ++out.nearN;

        const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
        const int info = ReadI32(drop, kOffDropInfo);
        if (money) {
            ++out.nearMoney;
            if (!haveMoneySample) {
                haveMoneySample = true;
                haveAnySample = true;
                out.sampleIsMoney = 1;
                out.sampleInfo = info;
            }
        } else {
            ++out.nearItem;
            if (!haveAnySample) {
                haveAnySample = true;
                out.sampleIsMoney = 0;
                out.sampleInfo = info;
            }
        }

        // 黑名单：不碰清闸/Stall；有写权限才盖长期戳
        if (haveSkip && DropMatchesSkip(drop, *skip)) {
            if (writesOk && StampOneSkipDrop(drop)) ++out.skipStamped;
            continue;
        }

        const int lastTry = ReadI32(drop, kOffDropLastTry);
        const int endPara = ReadI32(drop, kOffDropEndPara);
        const int own = ReadDropOwnType(drop);
        const int ownRaw = ReadDropOwnRaw(drop);
        const int ownerId = ReadI32(drop, kOffDropOwnerId);
        MaybeProbeDropOwnLayout(drop, endPara);
        // 优先用 Ready 样本（飞行中 OwnType 可能未写好）
        if (!gateSampled || (out.sampleEnd != kEndParaReady && endPara == kEndParaReady)) {
            gateSampled = true;
            out.sampleOwn = own;
            out.sampleOwnRaw = ownRaw;
            out.sampleOwnerId = ownerId;
            out.sampleLast = lastTry;
            out.sampleEnd = endPara;
        }

        // 非己/非无主：不进 ready/fly；Ready 时临时盖 LastTry 挡 ByPet（拍末还原）
        if (!DropClientPickable(drop)) {
            ++out.ownSkipped;
            if (writesOk && endPara == kEndParaReady && lastTry != kLastTrySkipStamp) {
                gOwnSkipStamps.push_back(OwnSkipStamp{drop, lastTry});
                WriteI32(drop, kOffDropLastTry, kLastTrySkipStamp);
                ++out.ownStamped;
            }
            continue;
        }

        if (ownerId > 0) {
            if (out.learnOwnerId == 0)
                out.learnOwnerId = ownerId;
            else if (out.learnOwnerId != ownerId)
                out.learnOwnerMixed = true;
        }

        // 仍在飞：与 ready 分开计。ed7ff1：仅 readyNear>0 就 ByPet → fly>0 仍 called=1，
        // 体感「物还没掉完宠物就吸」。黑名单已 continue，此处不含 SkipHold。
        if (endPara != kEndParaReady) ++out.flyNear;

        // 可被 ByPet 真正捡的：已落地 + Pickable + 非退避戳 + 己/无主
        const bool readyPick =
            endPara == kEndParaReady && ReadU8(drop, kOffDropPickable) != 0 &&
            lastTry != kLastTrySkipStamp;
        if (readyPick) ++out.readyNear;

        // 宠吸默认 maxClear=0：每拍清 LastTry/PickStamp 会让拒收件永远排队头，
        // 官方反复碰同一堆 → 地上落地动画打转（模块设计 §3.4 + BIN 2026-08-09）。
        // 拒收靠 StallActive / reject_backoff；冷却交给官方 PickStamp 窗。
        if (maxClear > 0 && out.gatesCleared < maxClear && readyPick) {
            if (ClearPickupGatesOne(drop, /*forceEndParaReady=*/false)) ++out.gatesCleared;
        }

        if (doStall) {
            const int did = ReadI32(drop, kOffDropId);
            if (StallActive(did, now) && ReadI32(drop, kOffDropLastTry) != kLastTrySkipStamp) {
                WriteI32(drop, kOffDropLastTry, kLastTrySkipStamp);
                ++out.stallStamped;
            }
        }
    }
    return out;
}

// 跨拍池下降：上一成功拍有 near/gates，本拍池变少（服端异步删 drop 的真吸证据）
bool PetVacPoolFellSinceLast(bool prevOk, DWORD prevTick, int prevDropAfter, int prevNear,
                             int prevGates, DWORD nowTick, int dropCount) {
    const bool recentPrev = prevOk && prevTick != 0 && (nowTick - prevTick) <= 3000u;
    return recentPrev && prevDropAfter > 0 && dropCount < prevDropAfter &&
           (prevNear > 0 || prevGates > 0);
}

// 飞物软挡（LastTry）已证伪：ByPet 仍会碰抛物中物（BIN touchMax=45 / fly>0+called=1），
// 盖戳反而可能拖落地。现行：盒内 flyNear>0 则不调 ByPet；并卸掉历史软挡戳。
std::unordered_map<int, int> gFlyPrevLast;  // 仅用于卸掉旧会话残留

void ClearFlyHolds() { gFlyPrevLast.clear(); }

// 卸掉池内仍挂着的飞物软挡（不论 EndPara）；旧 sticky 路径残留。
int AbandonAllFlyHolds(void* pool) {
    if (!pool || gFlyPrevLast.empty()) {
        ClearFlyHolds();
        return 0;
    }
    if (!DropWritesAllowed()) {
        ClearFlyHolds();
        return 0;
    }
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) {
        ClearFlyHolds();
        return 0;
    }
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) {
        ClearFlyHolds();
        return 0;
    }
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) {
        ClearFlyHolds();
        return 0;
    }

    int released = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        const int id = ReadI32(drop, kOffDropId);
        if (id == 0) continue;
        auto it = gFlyPrevLast.find(id);
        if (it == gFlyPrevLast.end()) continue;
        if (ReadI32(drop, kOffDropLastTry) == kLastTrySkipStamp) {
            WriteI32(drop, kOffDropLastTry, it->second);
            ++released;
        }
    }
    ClearFlyHolds();
    return released;
}

// 人吸选件前：若仍有记账，按盒还原已落地件（兼容切模式）。
int ReleaseLandedFlyHoldsNear(void* pool, float cx, float cy, float halfW, float halfH) {
    if (!pool || gFlyPrevLast.empty() || !DropWritesAllowed()) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int released = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float dpx = 0.f, dpy = 0.f;
        if (!ReadDropPt(drop, dpx, dpy)) continue;
        if (std::fabs(dpx - cx) > halfW || std::fabs(dpy - cy) > halfH) continue;
        const int id = ReadI32(drop, kOffDropId);
        if (id == 0) continue;
        auto it = gFlyPrevLast.find(id);
        if (it == gFlyPrevLast.end()) continue;
        if (ReadI32(drop, kOffDropEndPara) != kEndParaReady) continue;
        if (ReadI32(drop, kOffDropLastTry) == kLastTrySkipStamp) {
            WriteI32(drop, kOffDropLastTry, it->second);
            ++released;
        }
        gFlyPrevLast.erase(it);
    }
    return released;
}

bool ReadUserPos(float& x, float& y);

void RunVacuumOnMain() {
    VacuumResult& r = gJob.result;
    r = {};
    r.why = "fail";

    const DWORD now = GetTickCount();

    // 跨拍池下降 / 拒收整段休眠（满栏连打时跳过扫池+ByPet）
    static int s_prevDropAfter = -1;
    static int s_prevNear = 0;
    static int s_prevGates = 0;
    static DWORD s_prevTick = 0;
    static bool s_prevOk = false;
    static DWORD s_rejectBackoffUntil = 0;
    static int s_rejectStrikes = 0;
    constexpr int kRejectBackoffNeed = 3;
    constexpr DWORD kRejectBackoffMs = 5000;

    if (s_rejectBackoffUntil != 0 && now < s_rejectBackoffUntil) {
        r.why = "reject_backoff";
        r.ok = true;
        r.called = false;
        return;
    }
    if (s_rejectBackoffUntil != 0 && now >= s_rejectBackoffUntil) {
        s_rejectBackoffUntil = 0;
        gRejectBackoffUntil.store(0, std::memory_order_relaxed);
    }

    if (!ResolveLocalUser(now)) {
        r.why = "no_lu";
        return;
    }
    void* pool = ResolveDropPool(now);
    ResetStallIfPoolChanged(pool);
    PruneStall(now);
    // 混飞按件 Send 与人吸共用 pending Stall：下拍仍在池才算真拒收（同帧 Δ=0 多为异步真吸）
    if (gCharPendingStallId != 0) {
        const int pending = gCharPendingStallId;
        gCharPendingStallId = 0;
        if (pool && !gStallOff && DropIdInPool(pool, pending)) {
            AddStall(pending, now);
            if (s_rejectStrikes < 100) ++s_rejectStrikes;
            if (s_rejectStrikes >= kRejectBackoffNeed) {
                s_rejectBackoffUntil = now + kRejectBackoffMs;
                gRejectBackoffUntil.store(s_rejectBackoffUntil, std::memory_order_relaxed);
                s_rejectStrikes = 0;
                x::runtime::LogW(
                    "droppool",
                    "reject_backoff %ums (pending still in pool; 距离/归属/冷却，未必满栏)",
                    (unsigned)kRejectBackoffMs);
            }
        } else {
            s_rejectStrikes = 0;
        }
    }

    void* pet = FirstActivePet();
    if (!pet) {
        r.why = "no_pet";
        return;
    }

    if (!gPetKlass) gPetKlass = FindClass(kPetClass);
    const uint16_t skill = ReadPetSkill(pet);
    r.petSkill = skill;
    r.petSkillSlot = ReadPetSkillSlot(pet);
    if ((skill & kPetSkillPickupItem) == 0) {
        r.why = "no_skill";
        return;
    }

    float px = 0.f, py = 0.f;
    ReadPetPos(pet, px, py);
    r.dropCount = ReadPoolDropCount(pool);

    const float halfW = gJob.vacuumW * 0.5f;
    const float halfH = gJob.vacuumH * 0.5f;
    // 0：禁止拍前清闸（见 PreparePetVacNearPass 注释）；拒收靠 Stall / reject_backoff
    constexpr int kPetVacGateClearBudget = 0;
    const SkipIds* skipPtr = gJob.skip.empty() ? nullptr : &gJob.skip;

    auto finishEmptyLike = [&](DWORD nowTick, bool poolFell) {
        RefreshRemoteDropOwnersIfDue();
        r.remoteUsers = gRemotePeekOk ? gRemoteCountCached : -1;
        r.dropCountAfter = r.dropCount;
        r.dropsDelta = 0;
        r.poolFellSinceLast = poolFell;
        r.called = true;
        r.ok = true;
        r.why = poolFell ? "ok_absorbed" : "ok_empty";
        r.stallHeld = StallActiveCount(nowTick);
        r.beforeRc = ReadRect(pet, kOffPetRc);
        r.afterRc.x = -halfW;
        r.afterRc.y = -halfH;
        r.afterRc.w = gJob.vacuumW;
        r.afterRc.h = gJob.vacuumH;
        s_prevDropAfter = r.dropCountAfter;
        s_prevNear = r.nearCount;
        s_prevGates = r.gatesCleared;
        s_prevTick = nowTick;
        s_prevOk = true;
        if (poolFell) {
            s_rejectStrikes = 0;
            s_rejectBackoffUntil = 0;
            gRejectBackoffUntil.store(0, std::memory_order_relaxed);
        }
    };

    // 全图无掉落：O(1) 读 dict count，跳过 PreparePetVacNearPass 全表扫描
    if (!pool || r.dropCount <= 0) {
        ClearFlyHolds();
        if (!gJob.skip.empty() && pet) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);
        const DWORD nowTick = GetTickCount();
        const bool poolFell = PetVacPoolFellSinceLast(
            s_prevOk, s_prevTick, s_prevDropAfter, s_prevNear, s_prevGates, nowTick, r.dropCount);
        finishEmptyLike(nowTick, poolFell);
        return;
    }

    // 卸掉旧飞物软挡残留（不再盖戳）；再扫池。
    if (!gFlyPrevLast.empty()) (void)AbandonAllFlyHolds(pool);

    // 一次扫池：统计 +（有近物时）清闸/Stall/Skip；空盒无写并早退。
    const int stallActiveBefore = StallActiveCount(now);
    const PetVacNearPass pass =
        PreparePetVacNearPass(pool, px, py, halfW, halfH, now, skipPtr, kPetVacGateClearBudget);
    r.nearCount = pass.nearN;
    r.nearMoney = pass.nearMoney;
    r.nearItem = pass.nearItem;
    r.sampleIsMoney = pass.sampleIsMoney;
    r.sampleInfo = pass.sampleInfo;
    r.sampleOwnType = pass.sampleOwn;
    r.sampleOwnRaw = pass.sampleOwnRaw;
    r.sampleOwnerId = pass.sampleOwnerId;
    r.localCharId = static_cast<int>(LocalDropSelfOwnerId(
        pass.sampleOwnerId > 0 ? static_cast<uint32_t>(pass.sampleOwnerId) : 0));
    // 与预筛同拍：强制刷新一次远程人数（便于 mode=petmap 看 remotes=）
    RefreshRemoteDropOwnersIfDue();
    r.remoteUsers = gRemotePeekOk ? gRemoteCountCached : -1;
    r.sampleLastTry = pass.sampleLast;
    r.sampleEndPara = pass.sampleEnd;
    r.gatesCleared = pass.gatesCleared;
    r.stallStamped = pass.stallStamped;
    r.skipStamped = pass.skipStamped;
    r.ownSkipped = pass.ownSkipped;

    // 高价值优先：宠真空 ∩ 角色半盒 ∩ 栏未满 → 紧急；栏满跳过（不打断出刀）
    float hvUx = px, hvUy = py;
    const bool hvHaveUser = ReadUserPos(hvUx, hvUy);
    if (gJob.highValuePriority) {
        int hvN = 0, hvFull = 0;
        if (hvHaveUser) {
            const float charHalfW = gJob.vacuumW * 0.5f;
            const float charHalfH = gJob.vacuumH * 0.5f;
            ScanHighValueNear(pool, px, py, halfW, halfH, hvUx, hvUy, charHalfW, charHalfH,
                              skipPtr, hvN, hvFull, &r.highValueSampleDropId, &r.highValueSampleInfo,
                              &r.highValueSampleKind);
        }
        r.highValueNear = hvN;
        r.highValueSkippedFull = hvFull;
        r.highValueUrgent = hvN > 0;
    }

    if (r.nearCount == 0) {
        (void)RestoreOwnSkipStamps();
        if (!gJob.skip.empty()) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);
        const DWORD nowTick = GetTickCount();
        const bool poolFell = PetVacPoolFellSinceLast(
            s_prevOk, s_prevTick, s_prevDropAfter, s_prevNear, s_prevGates, nowTick, r.dropCount);
        finishEmptyLike(nowTick, poolFell);
        return;
    }

    // 按件 Pool.Send —— 仅宠吸路径上的安全阀，不用盒子尺寸切「人吸」：
    // 1) 混飞 flyNear>0：ByPet 会碰抛物中物
    // 2) 密堆 readyNear>ByPetReadyMax：ByPet 一拍连发 N 个 Pet.Send → BIN 静默 DC
    // 3) 高价值紧急：必须按件选装备/卷軸（金币让路）
    // 范围只扩 ByPet 矩形；人物直吸是面板另一档（TryCharVacuum），互不顶替。
    const bool pacedSend = r.highValueUrgent || pass.flyNear > 0 ||
                           pass.readyNear > xcat::kPetLootByPetReadyMax;
    if (pacedSend) {
        if (!gJob.skip.empty()) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);
        if (pool && r.stallStamped > 0) {
            r.stallRestored = RestoreStalledStampsNear(pool, px, py, halfW, halfH, now, skipPtr);
        }
        (void)RestoreOwnSkipStamps();

        EnsureSendProbe();
        auto fn = gOrigPoolSend ? reinterpret_cast<FnPoolSend>(gOrigPoolSend)
                                : FnFromMi<FnPoolSend>(gMiPoolSend, kRvaPoolSendDropPickUp);

        r.flyHeld = pass.flyNear;
        r.beforeRc = ReadRect(pet, kOffPetRc);
        r.afterRc.x = -halfW;
        r.afterRc.y = -halfH;
        r.afterRc.w = gJob.vacuumW;
        r.afterRc.h = gJob.vacuumH;

        if (!fn) {
            r.stallHeld = StallActiveCount(now);
            r.dropCountAfter = r.dropCount;
            r.dropsDelta = 0;
            r.called = false;
            r.ok = true;
            r.why = pass.flyNear > 0 ? "wait_land" : "ok_hold";
            return;
        }

        float ux = px, uy = py;
        if (!ReadUserPos(ux, uy)) {
            // 无角色坐标则宁可不送，避免宠 AbsPos 冒充 Maple 触发服端异常
            r.stallHeld = StallActiveCount(now);
            r.dropCountAfter = r.dropCount;
            r.dropsDelta = 0;
            r.called = false;
            r.ok = true;
            r.why = pass.flyNear > 0 ? "wait_land" : "ok_hold";
            return;
        }

        // 候选按用户真空盒；够不着由服端拒，不再代缩到 1500
        const float charHalfW = gJob.vacuumW * 0.5f;
        const float charHalfH = gJob.vacuumH * 0.5f;

        void* dict = ReadPtr(pool, kOffPoolDict);
        void* entries = LooksLikeHeapPtr(dict) ? ReadPtr(dict, kOffDictEntries) : nullptr;
        const int count = dict ? ReadI32(dict, kOffDictCount) : 0;
        const uintptr_t arrLen =
            (LooksLikeHeapPtr(entries) && count >= 0 && count <= 4096) ? ArrayLen(entries) : 0;

        void* bestDrop = nullptr;
        int bestId = 0;
        int bestInfo = 0;
        float bestD2 = 0.f;
        bool bestMoney = false;
        int bestHvRank = 0;  // 0=普通 1=金币 2=高价值
        HvClass bestHv = HvClass::None;
        int readyInPet = 0;  // 宠真空内 Ready（可能够不着）
        auto consider = [&]() {
            bestDrop = nullptr;
            bestId = 0;
            bestInfo = 0;
            bestD2 = 0.f;
            bestMoney = false;
            bestHvRank = 0;
            bestHv = HvClass::None;
            readyInPet = 0;
            for (uintptr_t i = 0; i < arrLen; ++i) {
                uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
                if (ReadI32(entry, kOffEntryHash) < 0) continue;
                void* drop = ReadPtr(entry, kOffEntryValue);
                if (!LooksLikeHeapPtr(drop)) continue;
                float dpx = 0.f, dpy = 0.f;
                if (!ReadDropPt(drop, dpx, dpy)) continue;
                if (std::fabs(dpx - px) > halfW || std::fabs(dpy - py) > halfH) continue;
                if (skipPtr && DropMatchesSkip(drop, *skipPtr)) continue;
                if (!DropClientPickable(drop)) continue;
                if (ReadU8(drop, kOffDropPickable) == 0) continue;
                if (ReadI32(drop, kOffDropEndPara) != kEndParaReady) continue;
                const int id = ReadI32(drop, kOffDropId);
                if (id == 0) continue;
                if (id == gCharPendingStallId) continue;
                // 拒收退避中：禁止再送（勿 ClearStall 强开，满栏时会立刻再撞 sentSame）
                if (StallActive(id, now)) continue;
                const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
                const int info = ReadI32(drop, kOffDropInfo);
                const HvClass hv = gJob.highValuePriority ? ClassifyHighValueItem(info, money)
                                                         : HvClass::None;
                // 高价值但对应栏满 → 跳过（用户要求：包满不吸）
                if (hv != HvClass::None && !HighValueBagAllows(hv)) continue;
                ++readyInPet;
                // 用户真空盒（角色中心）；超出由服端拒，不再另闸 750
                if (std::fabs(dpx - ux) > charHalfW || std::fabs(dpy - uy) > charHalfH) continue;
                const float dx = dpx - ux;
                const float dy = dpy - uy;
                const float d2 = dx * dx + dy * dy;
                // 优先级：高价值(2) > 金币(1) > 其它(0)；同档比距离
                int rank = 0;
                if (hv != HvClass::None)
                    rank = 2;
                else if (money)
                    rank = 1;
                const bool better =
                    !bestDrop || rank > bestHvRank || (rank == bestHvRank && d2 < bestD2);
                if (better) {
                    bestDrop = drop;
                    bestId = id;
                    bestInfo = info;
                    bestD2 = d2;
                    bestMoney = money;
                    bestHvRank = rank;
                    bestHv = hv;
                }
            }
        };
        consider();

        if (!bestDrop || bestId == 0) {
            r.stallHeld = StallActiveCount(now);
            r.dropCountAfter = r.dropCount;
            r.dropsDelta = 0;
            r.called = false;
            r.ok = true;
            // 无可达候选：不应继续紧急停刀（HV 在宠盒外/人够不着）
            if (gJob.highValuePriority) r.highValueUrgent = false;
            // 宠旁有 Ready 但角色够不着 → ok_far；全无 Ready → 飞中 wait_land / 落地 ok_hold
            if (readyInPet > 0)
                r.why = "ok_far";
            else if (pass.ownSkipped > 0 && pass.flyNear <= 0)
                r.why = "ok_own";
            else
                r.why = pass.flyNear > 0 ? "wait_land" : "ok_hold";
            return;
        }

        gCharPendingStallId = 0;

        r.pacedPickDropId = bestId;
        r.pacedPickInfo = bestInfo;
        r.pacedPickRank = bestHvRank;
        if (bestHvRank == 2 || r.highValueUrgent) {
            const char* nm = LookupItemNameBrief(bestInfo);
            x::runtime::LogI("droppool",
                             "paced pick dropId=%d info=%d rank=%d money=%d hv=%s name=%s", bestId,
                             bestInfo, bestHvRank, bestMoney ? 1 : 0, HvClassName(bestHv),
                             (nm && nm[0]) ? nm : "-");
        }

        if (DropWritesAllowed()) {
            const int last = ReadI32(bestDrop, kOffDropLastTry);
            const int stamp = ReadI32(bestDrop, kOffDropPickStamp);
            if (stamp != 0) WriteI32(bestDrop, kOffDropPickStamp, 0);
            if (last != 0) WriteI32(bestDrop, kOffDropLastTry, 0);
            r.gatesCleared = 1;
        }

        const int sentOwner = ReadI32(bestDrop, kOffDropOwnerId);
        int32_t pt[2] = {static_cast<int32_t>(ux), static_cast<int32_t>(uy)};
        bool seh = false;
        __try {
            fn(pool, pt, bestId, 0u, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            seh = true;
        }

        const DWORD nowTick = GetTickCount();
        r.dropCountAfter = ReadPoolDropCount(pool);
        r.dropsDelta = r.dropCountAfter - r.dropCount;
        const bool poolFell = PetVacPoolFellSinceLast(
            s_prevOk, s_prevTick, s_prevDropAfter, s_prevNear, s_prevGates, nowTick, r.dropCount);
        r.poolFellSinceLast = poolFell;
        r.called = !seh;
        r.ok = !seh;
        if (seh) {
            r.why = "seh";
        } else if (r.dropsDelta < 0 || poolFell) {
            r.why = "ok_absorbed";
            s_rejectStrikes = 0;
            s_rejectBackoffUntil = 0;
            gRejectBackoffUntil.store(0, std::memory_order_relaxed);
            if (sentOwner > 0) NoteDropSelfOwnerFromPickup(static_cast<uint32_t>(sentOwner));
        } else {
            r.why = "ok_sent";
            // 同帧池常未掉＝异步真吸常态；pending 下拍仍在才 AddStall / 计 reject
            if (!StallActive(bestId, nowTick)) gCharPendingStallId = bestId;
        }
        if (bestHvRank == 2 || r.highValueUrgent) {
            x::runtime::LogI("droppool",
                             "paced result dropId=%d info=%d rank=%d why=%s Δ=%d fell=%d", bestId,
                             bestInfo, bestHvRank, r.why ? r.why : "?", r.dropsDelta,
                             poolFell ? 1 : 0);
        }
        r.stallHeld = StallActiveCount(nowTick);
        if (!seh) {
            s_prevDropAfter = r.dropCountAfter;
            s_prevNear = r.nearCount;
            s_prevGates = r.gatesCleared;
            s_prevTick = nowTick;
            s_prevOk = true;
        }
        return;
    }

    // 无飞物：无 Ready 才等；有 Ready 且未超密堆顶 → 下方小批量 ByPet
    if (pass.readyNear <= 0) {
        if (!gJob.skip.empty()) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);
        if (pool && r.stallStamped > 0) {
            r.stallRestored = RestoreStalledStampsNear(pool, px, py, halfW, halfH, now, skipPtr);
        }
        (void)RestoreOwnSkipStamps();
        r.stallHeld = StallActiveCount(now);
        r.flyHeld = 0;
        r.dropCountAfter = r.dropCount;
        r.dropsDelta = 0;
        r.called = false;
        r.ok = true;
        r.why = (pass.ownSkipped > 0 && pass.flyNear <= 0) ? "ok_own" : "wait_land";
        r.beforeRc = ReadRect(pet, kOffPetRc);
        r.afterRc.x = -halfW;
        r.afterRc.y = -halfH;
        r.afterRc.w = gJob.vacuumW;
        r.afterRc.h = gJob.vacuumH;
        return;
    }

    // 不变量：Drop.Id 唯一 → 盒内被盖住的件数不可能超过生效退避条目数。
    if (r.stallStamped > stallActiveBefore) {
        gStallOff = true;
        x::runtime::LogW("droppool", "stall off: Drop.Id not unique (stamped=%d > held=%d)",
                         r.stallStamped, stallActiveBefore);
    }

    if (!gJob.skip.empty()) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);

    // 只读诊断：托管 _rcPet / CollisionCheck 对 ByPet Contains 无效，不再写入。
    r.beforeRc = ReadRect(pet, kOffPetRc);

    Rect4 vacuum{};
    vacuum.x = -halfW;
    vacuum.y = -halfH;
    vacuum.w = gJob.vacuumW;
    vacuum.h = gJob.vacuumH;
    // 日志 rc= 表示本拍意图真空尺寸（实际写入 .rdata 矩形包）
    r.afterRc = vacuum;

    EnsureSendProbe();  // 顺带 EnsureTryPickProbe：原生宠 Tick 走 TryPick MI
    EnsureTryPickProbe();

    r.flyHeld = 0;  // 能走到这里说明 flyNear==0

    ByPetRectBackup rectBak{};
    const bool rectPatched = PatchByPetRectPack(gJob.vacuumW, gJob.vacuumH, rectBak);

    auto fn = reinterpret_cast<FnPetTryPickUp>(gOrigTryPickUp);
    if (!fn) fn = FnFromMi<FnPetTryPickUp>(gMiTryPickUp, kRvaPetTryPickUpDrop);
    const uint32_t petHits0 = gPetSendHits.load(std::memory_order_relaxed);
    const uint32_t poolHits0 = gPoolSendHits.load(std::memory_order_relaxed);
    bool ok = false;
    __try {
        if (fn && rectPatched) {
            // 官方脚边同类调用 MethodInfo=nullptr；传 MI 在 CFF 下偶发早退
            fn(pet, nullptr);
            ok = true;
        } else if (fn && !rectPatched) {
            r.why = "no_rect_patch";
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        r.why = "seh";
    }

    RestoreByPetRectPack(rectBak);

    r.dropCountAfter = ReadPoolDropCount(pool);
    r.dropsDelta = r.dropCountAfter - r.dropCount;
    r.petSendHits = gPetSendHits.load(std::memory_order_relaxed);
    r.poolSendHits = gPoolSendHits.load(std::memory_order_relaxed);
    r.petSendDelta = r.petSendHits - petHits0;
    r.poolSendDelta = r.poolSendHits - poolHits0;

    // 服端异步删 drop：同拍前后常仍相等；用跨拍池下降作真吸证据
    const DWORD nowTick = GetTickCount();
    const bool poolFell = PetVacPoolFellSinceLast(s_prevOk, s_prevTick, s_prevDropAfter,
                                                  s_prevNear, s_prevGates, nowTick, r.dropCount);
    r.poolFellSinceLast = poolFell;

    // 仅在「调用成功且本拍池未同步下降」时轻扫盖戳，避免成功吸收再多走一趟
    if (ok && pool && r.nearCount > 0 && r.dropsDelta >= 0 && !poolFell) {
        int stallIds[16] = {};
        int stallN = 0;
        r.sendTouch = CountPostSendTouchesNear(pool, px, py, halfW, halfH, &r.sendTouchMoney,
                                               stallIds, 16, &stallN);
        const bool probeSend = r.petSendDelta > 0 || r.poolSendDelta > 0;
        if (r.sendTouch > 0 || probeSend) r.sentButPoolSame = 1;
        // 提交了但池没掉 → 多为该栏已满被服端拒收：登记退避，下一拍轮到别的掉落
        for (int i = 0; i < stallN; ++i) AddStall(stallIds[i], nowTick);
        // 官方拒收常不留可识别 LastTry/PickStamp → stallN=0 仍 sentSame：补登队头
        if (r.sentButPoolSame && stallN == 0) {
            (void)AddStallFallbackHeadsNear(pool, px, py, halfW, halfH, nowTick, skipPtr, 8);
        }
    }

    // 拍末必须把退避盖戳还原（含 seh / no_rect_patch 路径），否则切到脚边拾取时这些道具捡不起来
    if (pool && r.stallStamped > 0) {
        r.stallRestored = RestoreStalledStampsNear(pool, px, py, halfW, halfH, now, skipPtr);
    }
    (void)RestoreOwnSkipStamps();
    // 停用后才清表：清早了 RestoreStalledStampsNear 就找不到该还原谁，会把戳留在池里
    if (gStallOff && !gStall.empty()) gStall.clear();
    r.stallHeld = StallActiveCount(nowTick);

    r.called = ok;
    r.ok = ok;
    if (ok) {
        if (r.dropsDelta < 0 || poolFell)
            r.why = "ok_absorbed";
        else if (r.nearCount > 0)
            r.why = "ok";
        else
            r.why = "ok_empty";
    }

    if (ok) {
        s_prevDropAfter = r.dropCountAfter;
        s_prevNear = r.nearCount;
        s_prevGates = r.gatesCleared;
        s_prevTick = nowTick;
        s_prevOk = true;

        if (r.dropsDelta < 0 || poolFell) {
            s_rejectStrikes = 0;
            s_rejectBackoffUntil = 0;
            gRejectBackoffUntil.store(0, std::memory_order_relaxed);
            if (!pass.learnOwnerMixed && pass.learnOwnerId > 0)
                NoteDropSelfOwnerFromPickup(static_cast<uint32_t>(pass.learnOwnerId));
        } else if (r.sentButPoolSame) {
            if (s_rejectStrikes < 100) ++s_rejectStrikes;
            if (s_rejectStrikes >= kRejectBackoffNeed) {
                s_rejectBackoffUntil = nowTick + kRejectBackoffMs;
                gRejectBackoffUntil.store(s_rejectBackoffUntil, std::memory_order_relaxed);
                s_rejectStrikes = 0;
                x::runtime::LogW(
                    "droppool",
                    "reject_backoff %ums (sentSame streak → skip vac; 距离/归属/冷却，未必满栏)",
                    (unsigned)kRejectBackoffMs);
            }
        } else {
            s_rejectStrikes = 0;
        }
    }
}

void VacJobThunk(void*) {
    (void)x::runtime::main_thread::AssertOnPumpThread("drop.Vacuum");
    RunVacuumOnMain();
}

bool SkipHas(const SkipIds& skip, int id) { return skip.contains(id); }

bool DropMatchesSkip(void* drop, const SkipIds& skip) {
    if (!drop || skip.empty()) return false;
    const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
    if (money) return SkipHas(skip, kMesoSkipId);
    const int info = ReadI32(drop, kOffDropInfo);
    return info > 0 && SkipHas(skip, info);
}

bool InvHasFreeSlot(int invType) {
    void* list = x::ui::player::GetItemSlotList(invType);
    if (!LooksLikeHeapPtr(list)) return true;  // 读不到 → 不挡拾取
    const int n = ReadI32(list, kOffListSize);
    if (n <= 0 || n > 512) return true;
    void* items = ReadPtr(list, kOffListItems);
    if (!LooksLikeHeapPtr(items)) return true;
    const size_t idOff = x::ui::player::OffSlotItemId();
    if (!idOff) return true;
    const uintptr_t cap = ArrayLen(items);
    int used = 0;
    for (int i = 0; i < n && static_cast<uintptr_t>(i) < cap; ++i) {
        void* slot = ArrayAt(items, static_cast<uintptr_t>(i));
        if (!LooksLikeHeapPtr(slot)) continue;
        if (ReadI32(slot, idOff) > 0) ++used;
    }
    return used < n;
}

// 消耗/装饰/其他栏里某 itemId 的堆叠合计。纯字段读，worker 可调。
// 读不到栏表 → false（调用方不得把 0 当成「包里没有」）。
bool CountBagItem(int itemId, unsigned long long& out) {
    out = 0;
    if (itemId <= 0) return false;
    const size_t idOff = x::ui::player::OffSlotItemId();
    const size_t numOff = x::ui::player::OffSlotBundleNumber();
    if (!idOff) return false;
    bool any = false;
    for (const int type : {kInvTiConsume, kInvTiInstall, kInvTiEtc}) {
        void* list = x::ui::player::GetItemSlotList(type);
        if (!LooksLikeHeapPtr(list)) continue;
        const int n = ReadI32(list, kOffListSize);
        if (n <= 0 || n > 512) continue;
        void* items = ReadPtr(list, kOffListItems);
        if (!LooksLikeHeapPtr(items)) continue;
        any = true;
        const uintptr_t cap = ArrayLen(items);
        for (int i = 0; i < n && static_cast<uintptr_t>(i) < cap; ++i) {
            void* slot = ArrayAt(items, static_cast<uintptr_t>(i));
            if (!LooksLikeHeapPtr(slot)) continue;
            if (ReadI32(slot, idOff) != itemId) continue;
            const unsigned long long q = ReadU16(slot, numOff);
            out += q ? q : 1ull;
        }
    }
    return any;
}

HvClass ClassifyHighValueItem(int info, bool isMoney) {
    if (isMoney || info <= 0) return HvClass::None;
    // 装备：经典版 itemId / 1e6 == 1
    if (info / 1000000 == 1) return HvClass::Equip;
    // 只认 204 装备卷。回城/任务/洗点等品名带「卷轴」的消耗品不当高价值，也不叮咚。
    if (info >= 2040000 && info < 2050000) return HvClass::Scroll;
    // 雷之鏢：消耗栏飞镖，不是卷、也不是 1xxxxxx 装备。
    if (info == kThunderDartItemId) return HvClass::Dart;
    return HvClass::None;
}

bool HighValueBagAllows(HvClass hv) {
    if (hv == HvClass::Equip) return InvHasFreeSlot(kInvTiEquip);
    if (hv == HvClass::Scroll || hv == HvClass::Dart) return InvHasFreeSlot(kInvTiConsume);
    return false;
}

// 扫真空盒 ∩ 角色半盒：可吸高价值 / 栏满跳过数（纯内存；与 Send 同口径）
void ScanHighValueNear(void* pool, float petX, float petY, float halfW, float halfH, float ux,
                       float uy, float charHalfW, float charHalfH, const SkipIds* skip,
                       int& outNearHv, int& outSkippedFull, int* outSampleDropId,
                       int* outSampleInfo, int* outSampleKind) {
    outNearHv = 0;
    outSkippedFull = 0;
    if (outSampleDropId) *outSampleDropId = 0;
    if (outSampleInfo) *outSampleInfo = 0;
    if (outSampleKind) *outSampleKind = 0;
    if (!pool || charHalfW <= 0.f || charHalfH <= 0.f) return;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return;

    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float dpx = 0.f, dpy = 0.f;
        if (!ReadDropPt(drop, dpx, dpy)) continue;
        if (std::fabs(dpx - petX) > halfW || std::fabs(dpy - petY) > halfH) continue;
        if (skip && !skip->empty() && DropMatchesSkip(drop, *skip)) continue;
        if (!DropClientPickable(drop)) continue;
        if (ReadU8(drop, kOffDropPickable) == 0) continue;
        if (ReadI32(drop, kOffDropEndPara) != kEndParaReady) continue;
        const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
        const int info = ReadI32(drop, kOffDropInfo);
        const HvClass hv = ClassifyHighValueItem(info, money);
        if (hv == HvClass::None) continue;
        if (!HighValueBagAllows(hv)) {
            ++outSkippedFull;
            continue;
        }
        // 人吸盒外 → 不计入可吸（勿 urgent）；栏满已在上分支
        if (std::fabs(dpx - ux) > charHalfW || std::fabs(dpy - uy) > charHalfH) continue;
        if (outNearHv == 0) {
            if (outSampleDropId) *outSampleDropId = ReadI32(drop, kOffDropId);
            if (outSampleInfo) *outSampleInfo = info;
            if (outSampleKind) *outSampleKind = static_cast<int>(hv);
        }
        ++outNearHv;
    }
}

bool ReadUserPos(float& x, float& y) {
    x = y = 0.f;
    if (!gLocalUser) return false;

    // 脚边 pos / 黑名单盖戳须对齐 Drop.Pt1（Maple Y-down）。
    // Ap：Unity Y-up → 翻 Y；CurPos@0x240 已是 Maple 空间，不翻。
    void* vc = ReadPtr(gLocalUser, kOffVecCtrl);
    if (LooksLikeHeapPtr(vc)) {
        const double ax = ReadF64(vc, kOffVcApX);
        const double ay = ReadF64(vc, kOffVcApY);
        if (std::fabs(ax) >= kMinPosAbs || std::fabs(ay) >= kMinPosAbs) {
            ApToMaplePos(ax, ay, x, y);
            return true;
        }
    }

    x = ReadF32(gLocalUser, kOffCurPos);
    y = ReadF32(gLocalUser, kOffCurPos + 4);
    if (std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs) return true;

    const float ux = ReadF32(gLocalUser, kOffFieldPos);
    const float uy = ReadF32(gLocalUser, kOffFieldPos + 4);
    x = ux;
    y = -uy;
    return std::fabs(x) >= kMinPosAbs || std::fabs(y) >= kMinPosAbs;
}

// 池内距 (cx,cy) 最近的 Drop.Pt1（不限真空盒；probe 诊断用）
bool SampleNearestDropPt(void* pool, float cx, float cy, float& outX, float& outY, float& outDist) {
    outX = outY = 0.f;
    outDist = -1.f;
    if (!pool) return false;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return false;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return false;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return false;

    bool found = false;
    float bestD2 = 0.f;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        const float dx = px - cx;
        const float dy = py - cy;
        const float d2 = dx * dx + dy * dy;
        if (!found || d2 < bestD2) {
            found = true;
            bestD2 = d2;
            outX = px;
            outY = py;
        }
    }
    if (!found) return false;
    outDist = std::sqrt(bestD2);
    return true;
}

// 黑名单：LastTry=INT_MAX + EndPara=SkipHold(!=3) → ByPet 早退，避免空吸打转
int StampSkippedDropsNear(void* pool, float cx, float cy, float halfW, float halfH,
                          const SkipIds& skip, int* outNear, int* outWant, int* outTotal) {
    if (outNear) *outNear = 0;
    if (outWant) *outWant = 0;
    if (outTotal) *outTotal = 0;
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int total = 0, nearN = 0, want = 0, stamped = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        const int hash = ReadI32(entry, kOffEntryHash);
        if (hash < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        ++total;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        const float dx = px - cx;
        const float dy = py - cy;
        if (std::fabs(dx) > halfW || std::fabs(dy) > halfH) continue;
        ++nearN;
        if (DropMatchesSkip(drop, skip)) {
            if (StampOneSkipDrop(drop)) ++stamped;
            continue;
        }
        ++want;
    }
    if (outNear) *outNear = nearN;
    if (outWant) *outWant = want;
    if (outTotal) *outTotal = total;
    return stamped;
}

// 退避黑名单：盖 LastTry=INT_MAX（拍末还原）。不动 EndPara，避免拍内 3↔4 重播落地。
// 拍内对称（与 .rdata 矩形包同范式）：这里盖、拍末 RestoreStalledStampsNear 恢复。
// 绝不跨拍留戳——否则用户切到脚边拾取后，这些他想要的道具会一直捡不起来。
int StampStalledDropsNear(void* pool, float cx, float cy, float halfW, float halfH, DWORD now,
                          const SkipIds* skip) {
    if (!pool || gStallOff || gStall.empty() || !DropWritesAllowed()) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int stamped = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        if (!StallActive(ReadI32(drop, kOffDropId), now)) continue;
        // 用户黑名单的戳由 StampSkippedDropsNear 长期负责，别掺和进拍内恢复
        if (skip && !skip->empty() && DropMatchesSkip(drop, *skip)) continue;

        bool touched = false;
        if (ReadI32(drop, kOffDropLastTry) != kLastTrySkipStamp) {
            WriteI32(drop, kOffDropLastTry, kLastTrySkipStamp);
            touched = true;
        }
        // 只盖 LastTry：勿动 EndPara。拍内 3→4→还原 3 会反复触发落地动画
        // （人物直吸已因此去掉 Stamp；宠吸退避同理只挡 ByPet 队头）。
        if (touched) ++stamped;
    }
    return stamped;
}

// 拍末恢复退避盖戳：重新枚举字典（不跨托管调用持裸 drop 指针），把本拍写下的
// LastTry=INT_MAX 还原成 0。不再动 EndPara（避免 4↔3 重播落地动画）。
// 故意不加 DropWritesAllowed 门禁：只写「读回来正好是 INT_MAX」的 drop，且盖戳侧已被门禁挡住
// （没盖就没得还原）。若这里也加门禁，写权限在拍内翻假就会把戳永久留在池里。
int RestoreStalledStampsNear(void* pool, float cx, float cy, float halfW, float halfH, DWORD now,
                             const SkipIds* skip) {
    if (!pool || gStall.empty()) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int restored = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - cx) > halfW || std::fabs(py - cy) > halfH) continue;
        if (!StallActive(ReadI32(drop, kOffDropId), now)) continue;
        if (skip && !skip->empty() && DropMatchesSkip(drop, *skip)) continue;
        if (ReadI32(drop, kOffDropLastTry) != kLastTrySkipStamp) continue;

        WriteI32(drop, kOffDropLastTry, 0);
        ++restored;
    }
    return restored;
}

void RunFootOnMain() {
    FootResult& r = gFoot.result;
    r = {};
    r.why = "fail";

    const DWORD now = GetTickCount();
    if (!ResolveLocalUser(now)) {
        r.why = "no_lu";
        return;
    }
    void* pool = ResolveDropPool(now);
    if (!pool) {
        r.why = "no_pool";
        return;
    }
    EnsureSendProbe();

    float ux = 0.f, uy = 0.f;
    if (!ReadUserPos(ux, uy)) {
        r.why = "no_pos";
        return;
    }
    r.userX = ux;
    r.userY = uy;
    r.dropCount = ReadPoolDropCount(pool);

    if (!gDropPoolKlass) gDropPoolKlass = FindClass(kDropPoolClass);
    if (!gMiFootTryPickUp && gDropPoolKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        // void(in Vector2) 唯一。
        constexpr MethodShape kFoot{1, TypeKind::Void, true, false, {TypeKind::Any}};
        gMiFootTryPickUp = ResolveMi(gDropPoolKlass, kRvaDropTryPickUpDrop, kFoot, "TryPickUpDrop",
                                     kHashDropTryPickUp);
    }
    auto fn = FnFromMi<FnDropTryPickUp>(gMiFootTryPickUp, kRvaDropTryPickUpDrop);
    // 官方入口收的是 Unity 空间（Y-up）：函数内把矩形和 PickPt 都按 −Y 比，送包时再 Point(x, −y)。
    // ReadUserPos 给的是 Maple（Y-down），所以这里必须翻 Y，否则盒子落在 −y 侧，脚边永远空吸。
    float pos[2] = {ux, -uy};
    const uint32_t petHits0 = gPetSendHits.load(std::memory_order_relaxed);
    const uint32_t poolHits0 = gPoolSendHits.load(std::memory_order_relaxed);
    bool ok = false;
    __try {
        if (fn) {
            // 脚下走 DropPool 口：传解析到的 MI（与旧脚边一致；宠吸 Pet 口才用 nullptr 躲 CFF）
            fn(pool, pos, gMiFootTryPickUp);
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        r.why = "seh";
    }
    r.dropCountAfter = ReadPoolDropCount(pool);
    r.dropsDelta = r.dropCountAfter - r.dropCount;
    r.petSendHits = gPetSendHits.load(std::memory_order_relaxed);
    r.poolSendHits = gPoolSendHits.load(std::memory_order_relaxed);
    r.petSendDelta = r.petSendHits - petHits0;
    r.poolSendDelta = r.poolSendHits - poolHits0;
    r.called = ok;
    r.ok = ok;

    // 跨拍吸收：须上一拍有 pool Send（避免远处自然消/他人捡误记 ok_absorbed）
    static int s_footPrevAfter = -1;
    static uint32_t s_footPrevPoolSendDelta = 0;
    static DWORD s_footPrevTick = 0;
    static bool s_footPrevOk = false;
    const DWORD nowTick = GetTickCount();
    const bool recentPrev =
        s_footPrevOk && s_footPrevTick != 0 && (nowTick - s_footPrevTick) <= 3000u;
    const bool poolFell = recentPrev && s_footPrevAfter > 0 && r.dropCount < s_footPrevAfter &&
                          s_footPrevPoolSendDelta > 0;
    r.poolFellSinceLast = poolFell;

    if (ok) {
        if (r.dropsDelta < 0 || poolFell) r.why = "ok_absorbed";
        else if (r.dropCount > 0)
            r.why = "ok_called";  // 已触发；池里仍有物（未必在盒内 / 未必捡到）
        else
            r.why = "ok_empty";
    } else if (!r.why || !r.why[0] || strcmp(r.why, "fail") == 0) {
        r.why = "no_fn";
    }

    if (ok) {
        s_footPrevAfter = r.dropCountAfter;
        s_footPrevPoolSendDelta = r.poolSendDelta;
        s_footPrevTick = nowTick;
        s_footPrevOk = true;
    }
}

void FootJobThunk(void*) {
    (void)x::runtime::main_thread::AssertOnPumpThread("drop.Foot");
    RunFootOnMain();
}

void RunCharVacOnMain() {
    CharVacResult& r = gCharVac.result;
    r = {};
    r.why = "fail";

    const DWORD now = GetTickCount();
    if (!ResolveLocalUser(now)) {
        r.why = "no_lu";
        return;
    }
    void* pool = ResolveDropPool(now);
    if (!pool) {
        r.why = "no_pool";
        return;
    }
    EnsureSendProbe();
    // 读 Pt1/Pickable/Id/EndPara 错址会乱选包；写路径另受 DropWritesAllowed 门禁
    if (!gCharGatesMeta) {
        r.why = "no_meta";
        return;
    }

    float ux = 0.f, uy = 0.f;
    if (!ReadUserPos(ux, uy)) {
        r.why = "no_pos";
        return;
    }
    r.userX = ux;
    r.userY = uy;

    // 走原始 Send，别把自己的包记进 poolSendHits 探针
    auto fn = gOrigPoolSend ? reinterpret_cast<FnPoolSend>(gOrigPoolSend)
                            : FnFromMi<FnPoolSend>(gMiPoolSend, kRvaPoolSendDropPickUp);
    if (!fn) {
        r.why = "no_fn";
        return;
    }

    // —— 以下控制面与 RunVacuumOnMain 同构，中心从宠坐标换成角色坐标 ——
    ResetStallIfPoolChanged(pool);
    PruneStall(now);

    // 上一拍送了、池可能尚未同步：若 drop 仍在 → 确认拒收并 AddStall；已不在 → 真吸，不误伤
    if (gCharPendingStallId != 0) {
        const int pending = gCharPendingStallId;
        gCharPendingStallId = 0;
        if (!gStallOff && DropIdInPool(pool, pending)) {
            AddStall(pending, now);
            r.sentButPoolSame = 1;
        }
    }

    // 半盒 = 用户真空 / 2，不再另顶 750
    float halfW = gCharVac.halfW;
    float halfH = gCharVac.halfH;
    const SkipIds* skip = gCharVac.skip.empty() ? nullptr : &gCharVac.skip;

    r.dropCount = ReadPoolDropCount(pool);
    int totalEnum = 0;
    r.nearCount =
        CountDropsNear(pool, ux, uy, halfW, halfH, &totalEnum, nullptr, nullptr, nullptr, nullptr);
    if (r.dropCount <= 0 && totalEnum > 0) r.dropCount = totalEnum;

    // 不调全盒 ClearPickupGatesNear：大盒 near 多时每拍上百次写会占死 MainPump，
    // 打怪/瞬移/攻击加速同泵排队 → 日志见 fires 归零 + drain budget deferred。
    r.gatesCleared = 0;

    // 人物直吸自己选 id 再 Send：软件 StallActive / DropMatchesSkip 已够。
    // 禁止照搬宠吸的 Stamp+Restore（盒内数十件每拍 EndPara 3↔4 → 反复落地动画，
    // 与宠吸早期「清闸回写 EndPara=3 打转」同一类）。
    r.stallStamped = 0;
    r.skipStamped = 0;

    // 送包坐标 = 角色真实 Maple 位置（官方人物拾取链同款）
    int32_t pt[2] = {static_cast<int32_t>(ux), static_cast<int32_t>(uy)};
    bool seh = false;

    void* dict = ReadPtr(pool, kOffPoolDict);
    void* entries = LooksLikeHeapPtr(dict) ? ReadPtr(dict, kOffDictEntries) : nullptr;
    const int count = dict ? ReadI32(dict, kOffDictCount) : 0;
    const uintptr_t arrLen =
        (LooksLikeHeapPtr(entries) && count >= 0 && count <= 4096) ? ArrayLen(entries) : 0;

    // 一调一件，且只吸人物最近的 Ready drop（盒内按距离² 选最近）。
    // 抛物未落地(EndPara!=3)一律不写字段——065ed0：先 Clear 再判 EndPara 会扫多件并
    // 清 LastTry/PickStamp，落地动画打转。只对选中那一件清冷却后 Send。
    // 若刚从宠吸切来：释放已落地的飞物软挡，避免 LastTry=INT_MAX 被人吸永久跳过。
    (void)ReleaseLandedFlyHoldsNear(pool, ux, uy, halfW, halfH);

    void* bestDrop = nullptr;
    int bestId = 0;
    float bestD2 = 0.f;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;

        float px = 0.f, py = 0.f;
        if (!ReadDropPt(drop, px, py)) continue;
        if (std::fabs(px - ux) > halfW || std::fabs(py - uy) > halfH) continue;

        if (ReadU8(drop, kOffDropPickable) == 0) {
            ++r.gateBlocked;
            continue;
        }
        if (skip && DropMatchesSkip(drop, *skip)) continue;
        if (!DropClientPickable(drop)) continue;

        const int id = ReadI32(drop, kOffDropId);
        if (id == 0) continue;
        if (StallActive(id, now) || id == gCharPendingStallId) continue;
        if (ReadI32(drop, kOffDropLastTry) == kLastTrySkipStamp) continue;
        if (ReadI32(drop, kOffDropEndPara) != kEndParaReady) continue;

        const float dx = px - ux;
        const float dy = py - uy;
        const float d2 = dx * dx + dy * dy;
        if (!bestDrop || d2 < bestD2) {
            bestDrop = drop;
            bestId = id;
            bestD2 = d2;
        }
    }

    if (bestDrop && bestId != 0) {
        // 仅清冷却戳，便于官方 Send 过 PickStamp 窗；绝不碰 EndPara
        if (DropWritesAllowed()) {
            bool touched = false;
            const int last = ReadI32(bestDrop, kOffDropLastTry);
            const int stamp = ReadI32(bestDrop, kOffDropPickStamp);
            if (stamp != 0) {
                WriteI32(bestDrop, kOffDropPickStamp, 0);
                touched = true;
            }
            if (last != 0 && last != kLastTrySkipStamp) {
                WriteI32(bestDrop, kOffDropLastTry, 0);
                touched = true;
            }
            if (touched) ++r.gatesCleared;
        }
        ++r.nearWant;

        const int sentOwner = ReadI32(bestDrop, kOffDropOwnerId);
        __try {
            fn(pool, pt, bestId, 0u, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            seh = true;
        }
        if (!seh) {
            ++r.sent;
            r.sentDropId = bestId;
        }
        gCharPendingLearnOwner = (!seh && sentOwner > 0) ? static_cast<uint32_t>(sentOwner) : 0;
    } else {
        gCharPendingLearnOwner = 0;
    }

    gCharSentTotal.fetch_add(static_cast<uint32_t>(r.sent), std::memory_order_relaxed);
    r.sentTotal = gCharSentTotal.load(std::memory_order_relaxed);
    r.dropCountAfter = ReadPoolDropCount(pool);
    r.dropsDelta = r.dropCountAfter - r.dropCount;

    static int s_prevAfter = -1;
    static int s_prevNear = 0;
    static int s_prevSent = 0;
    static DWORD s_prevTick = 0;
    static bool s_prevOk = false;
    const DWORD nowTick = GetTickCount();
    const bool recentPrev = s_prevOk && s_prevTick != 0 && (nowTick - s_prevTick) <= 3000u;
    const bool poolFell =
        recentPrev && s_prevAfter > 0 && r.dropCount < s_prevAfter && (s_prevNear > 0 || s_prevSent > 0);
    r.poolFellSinceLast = poolFell;

    // 人物 Send 通常不写 LastTry。池未掉 → 只对「本拍送出的 id」pending/AddStall。
    // 禁止 CountPostSendTouchesNear 整盒登记：盒内残留 LastTry 会把无关物全打进退避
    // （065ed0：sendTouch=90 而 gates=1），与动画/吸物抖动无关但也是脏负载。
    if (!seh && r.sent > 0 && r.nearCount > 0 && r.dropsDelta >= 0 && !poolFell) {
        if (r.sentDropId != 0) {
            if (!StallActive(r.sentDropId, nowTick)) gCharPendingStallId = r.sentDropId;
        }
    } else if (!seh && r.sent > 0 && (r.dropsDelta < 0 || poolFell)) {
        gCharPendingStallId = 0;
        if (gCharPendingLearnOwner) NoteDropSelfOwnerFromPickup(gCharPendingLearnOwner);
        gCharPendingLearnOwner = 0;
    }

    // 人物直吸不盖/不还原 EndPara 戳（见上）；stallHeld 仍反映软件退避表
    if (gStallOff && !gStall.empty()) gStall.clear();
    r.stallRestored = 0;
    r.stallHeld = StallActiveCount(nowTick);

    r.called = true;
    r.ok = !seh;
    if (seh) {
        r.why = "seh";
    } else if (r.dropsDelta < 0 || poolFell) {
        r.why = "ok_absorbed";
        if (gCharPendingLearnOwner) NoteDropSelfOwnerFromPickup(gCharPendingLearnOwner);
        gCharPendingLearnOwner = 0;
    } else if (r.sent > 0) {
        r.why = r.sentButPoolSame ? "ok" : "ok_sent";
    } else if (r.stallHeld > 0) {
        r.why = "ok_hold";
    } else if (r.nearCount == 0) {
        r.why = r.dropCount > 0 ? "ok_far" : "ok_empty";
    } else if (r.gateBlocked > 0 && r.nearWant == 0) {
        r.why = "ok_gate";
    } else {
        r.why = "ok";
    }

    if (!seh) {
        s_prevAfter = r.dropCountAfter;
        s_prevNear = r.nearCount;
        s_prevSent = r.sent;
        s_prevTick = nowTick;
        s_prevOk = true;
    }
}

void CharVacJobThunk(void*) {
    (void)x::runtime::main_thread::AssertOnPumpThread("drop.CharVac");
    RunCharVacOnMain();
}

}  // namespace

void Init() {
    BindIl2Cpp();
    (void)ResolveByPetRectPack(false);
    x::runtime::LogI("DropPort", "init pet_loot port (pet vacuum; foot=native TryPickUpDrop only)");
}

void Shutdown() {
    ReleaseByPetRectPackImpl();
    for (int i = 0; i < gTryPickVtN; ++i) {
        RestoreVtableMethodPtr(gTryPickVtSlot[i], gTryPickVtOrig[i]);
        gTryPickVtSlot[i] = nullptr;
        gTryPickVtOrig[i] = nullptr;
    }
    gTryPickVtN = 0;
    for (int i = 0; i < gTryPickMiN; ++i) {
        RestoreMethodInfo(gTryPickMi[i].mi, gTryPickMi[i].orig);
        gTryPickMi[i] = {};
    }
    gTryPickMiN = 0;
    if (gMiPetSend && gOrigPetSend) RestoreMethodInfo(gMiPetSend, gOrigPetSend);
    if (gMiPoolSend && gOrigPoolSend) RestoreMethodInfo(gMiPoolSend, gOrigPoolSend);
    gMiPetSend = nullptr;
    gMiPoolSend = nullptr;
    gMiTryPickUp = nullptr;
    gOrigPetSend = nullptr;
    gOrigPoolSend = nullptr;
    gOrigTryPickUp = nullptr;
    gSendProbeInstalled.store(false, std::memory_order_release);
    gTryPickProbeInstalled.store(false, std::memory_order_release);
    gDropPool = nullptr;
    gLocalUser = nullptr;
}

bool HoldByPetRectPack(float vacuumW, float vacuumH) {
    return HoldByPetRectPackImpl(vacuumW, vacuumH);
}

void ReleaseByPetRectPack() { ReleaseByPetRectPackImpl(); }

bool EnsureBound() {
    if (!BindIl2Cpp()) return false;
    const DWORD now = GetTickCount();
    const bool lu = ResolveLocalUser(now);
    void* pool = ResolveDropPool(now);
    if (!gPetKlass) gPetKlass = FindClass(kPetClass);
    if (lu && pool) {
        EnsureSendProbe();
        EnsureTryPickProbe();
    }
    return lu && pool != nullptr;
}

void* PeekLocalUser() { return gLocalUser; }
void* PeekDropPool() { return gDropPool; }

bool CollectProbe(ProbeSnapshot& out, float nearHalfW, float nearHalfH) {
    out = {};
    if (!EnsureBound()) return false;
    const DWORD now = GetTickCount();
    void* pool = ResolveDropPool(now);
    void* pet = FirstActivePet();
    out.hasPet = pet != nullptr;
    out.pet = pet;
    if (pet) {
        out.petSkill = ReadPetSkill(pet);
        out.petRc = ReadRect(pet, kOffPetRc);
        ReadPetPos(pet, out.petX, out.petY);
    }
    out.collisionRcPet = ReadCollisionRcPet();
    if (pool && pet) {
        out.nearCount =
            CountDropsNear(pool, out.petX, out.petY, nearHalfW, nearHalfH, &out.dropCount);
        out.hasSampleDrop = SampleNearestDropPt(pool, out.petX, out.petY, out.sampleDropX,
                                               out.sampleDropY, out.sampleDropDist);
    } else if (pool) {
        CountDropsNear(pool, 0.f, 0.f, 1e9f, 1e9f, &out.dropCount);
        // 无宠时以原点采样，仅给 drops 对照；dist 无意义
        out.hasSampleDrop =
            SampleNearestDropPt(pool, 0.f, 0.f, out.sampleDropX, out.sampleDropY, out.sampleDropDist);
        if (out.hasSampleDrop) out.sampleDropDist = -1.f;
    }
    out.ok = true;
    return true;
}

int BoostDropFall(bool snapLand, bool accelFall, const SkipIds* skipIds, int* outSnap,
                  int* outAccel, int* outSkipHold) {
    if (outSnap) *outSnap = 0;
    if (outAccel) *outAccel = 0;
    if (outSkipHold) *outSkipHold = 0;
    if (!snapLand && !accelFall) return 0;
    if (!EnsureBound() || !DropWritesAllowed()) return 0;
    void* pool = ResolveDropPool(GetTickCount());
    if (!pool) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    // Drop.Update：t = LastTry/1000；态1 每帧 +0x90 += 36。worker 约 40ms 一拍。
    constexpr int kAccelStep = 200;
    constexpr int kAccelCap = 1000;
    constexpr double kSpinStep = 36.0;
    constexpr int kSpinCopies = 5;
    constexpr size_t kFbDropSpin = 0x90;
    const bool spinOk =
        kOffDropPickPt == kFbDropPickPt && kOffDropLastTry == kFbDropLastTry;
    const bool haveSkip = skipIds && !skipIds->empty();

    int snapN = 0;
    int accelN = 0;
    int skipHoldN = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;

        const int last = ReadI32(drop, kOffDropLastTry);
        const int endp = ReadI32(drop, kOffDropEndPara);
        const int px = ReadI32(drop, kOffDropPickPt);
        const int py = ReadI32(drop, kOffDropPickPt + 4);
        const bool havePickPt = !(px == 0 && py == 0);
        const bool skipHit = haveSkip && DropMatchesSkip(drop, *skipIds);
        const bool money = ReadU8(drop, kOffDropIsMoney) != 0;
        const int info = ReadI32(drop, kOffDropInfo);

        if (skipHit) {
            // 禁止跟位置。A124：info=2060000 已匹配并盖 EndPara=4，再写 Pt1=PickPt
            // 会让 Drop.Update 把态写回 Ready(3)+可捡，下一拍再 3→4，瞬落空档被原生宠舔走。
            if (StampOneSkipDrop(drop)) ++skipHoldN;
            continue;
        }

        if (endp == kEndParaSkipHold) continue;
        if (endp == kEndParaReady) continue;
        if (endp < 0 || endp > 2) continue;
        if (!money && info <= 0) continue;
        if (last == kLastTrySkipStamp) continue;
        if (!havePickPt) continue;

        if (snapLand) {
            WriteI32(drop, kOffDropPt1, px);
            WriteI32(drop, kOffDropPt1 + 4, py);
            WriteI32(drop, kOffDropEndPara, kEndParaReady);
            WriteI32(drop, kOffDropLastTry, 0);
            const int stamp = ReadI32(drop, kOffDropPickStamp);
            if (stamp != 0) WriteI32(drop, kOffDropPickStamp, 0);
            ++snapN;
            continue;
        }

        if (!accelFall) continue;
        if (last >= 0 && last < kAccelCap) {
            int next = last + kAccelStep;
            if (next > kAccelCap) next = kAccelCap;
            if (next != last) WriteI32(drop, kOffDropLastTry, next);
        }
        if (spinOk) {
            const double spin = ReadF64(drop, kFbDropSpin);
            WriteF64(drop, kFbDropSpin, spin + kSpinStep * static_cast<double>(kSpinCopies));
        }
        ++accelN;
    }
    if (outSnap) *outSnap = snapN;
    if (outAccel) *outAccel = accelN;
    if (outSkipHold) *outSkipHold = skipHoldN;
    return snapN + accelN + skipHoldN;
}

int HoldSkipDrops(const SkipIds* skipIds, float halfW, float halfH) {
    if (!skipIds || skipIds->empty()) {
        PublishLiveSkip(nullptr);
        return 0;
    }
    PublishLiveSkip(skipIds);
    (void)halfW;
    (void)halfH;
    if (!EnsureBound()) return 0;
    EnsureSendProbe();
    const DWORD now = GetTickCount();
    void* pool = ResolveDropPool(now);
    if (!pool) return 0;
    // ExceptionList 需要宠对象；盖戳本身不依赖宠坐标——原生 50x60 在脚边，
    // 只按宠真空盒盖会漏。全池盖，与 TryPick 钩 StampSkipBeforeNativePick 对齐。
    void* pet = FirstActivePet();
    if (pet) (void)EnsureExceptionIdsIfNeeded(pet, *skipIds);
    return StampSkippedDropsNear(pool, 0.f, 0.f, 1e9f, 1e9f, *skipIds, nullptr, nullptr, nullptr);
}

bool TryPetVacuum(float vacuumW, float vacuumH, const SkipIds* skipIds, VacuumResult& out,
                  bool highValuePriority) {
    out = {};
    if (!EnsureBound()) {
        out.why = "unbound";
        return false;
    }
    if (!(vacuumW > 1.f) || !(vacuumH > 1.f)) {
        out.why = "bad_box";
        return false;
    }

    const DWORD backoffUntil = gRejectBackoffUntil.load(std::memory_order_relaxed);
    if (backoffUntil != 0 && GetTickCount() < backoffUntil) {
        out.why = "reject_backoff";
        out.ok = true;
        out.called = false;
        return true;
    }

    gJob.vacuumW = vacuumW;
    gJob.vacuumH = vacuumH;
    gJob.highValuePriority = highValuePriority;
    gJob.skip = {};
    if (skipIds) gJob.skip = *skipIds;
    PublishLiveSkip(skipIds);
    gJob.result = {};
    gJob.done = false;

    if (!x::runtime::main_thread::Ensure()) {
        out.why = "no_pump";
        return false;
    }
    if (!x::runtime::main_thread::InvokeAndWait(&VacJobThunk, nullptr, kJobWaitMs,
                                               x::runtime::main_thread::JobPrio::Low)) {
        out.why = "timeout";
        return false;
    }
    out = gJob.result;
    return out.ok;
}

bool PeekHighValueActionable(float petX, float petY, float halfW, float halfH, const SkipIds* skip,
                             int& outNearHv, int& outSkippedFull, int* outSampleDropId,
                             int* outSampleInfo, int* outSampleKind) {
    outNearHv = 0;
    outSkippedFull = 0;
    if (outSampleDropId) *outSampleDropId = 0;
    if (outSampleInfo) *outSampleInfo = 0;
    if (outSampleKind) *outSampleKind = 0;
    if (!EnsureBound()) return false;
    void* pool = PeekDropPool();
    if (!LooksLikeHeapPtr(pool)) pool = ResolveDropPool(GetTickCount());
    if (!LooksLikeHeapPtr(pool)) return false;
    if (!ResolveLocalUser(GetTickCount())) return false;
    float ux = 0.f, uy = 0.f;
    if (!ReadUserPos(ux, uy)) return false;
    const float charHalfW = halfW;
    const float charHalfH = halfH;
    ScanHighValueNear(pool, petX, petY, halfW, halfH, ux, uy, charHalfW, charHalfH, skip,
                      outNearHv, outSkippedFull, outSampleDropId, outSampleInfo, outSampleKind);
    return true;
}

namespace {
struct HighValueWatch {
    int itemId = 0;
    int kind = 0;
    bool published = false;
    bool bagKnown = false;
    DWORD publishedMs = 0;
    DWORD missingSince = 0;  // 0=本拍仍在池里
    unsigned long long bagAtPublish = 0;
};

void* gHvAlertPool = nullptr;
std::unordered_map<int, HighValueWatch> gHvWatch;
std::vector<HighValueDropAlert> gHvGone;
bool gHvSeeded = false;
constexpr DWORD kHvPickSuccessMaxAgeMs = 180000;
constexpr DWORD kHvGoneDebounceMs = 400;   // 字典闪漏 / 瞬落改写不稳
constexpr DWORD kHvGoneGiveUpMs = 15000;   // 没进包：别人捡了或过期，静默丢掉
constexpr size_t kHvGoneCap = 16;

void ResetHighValueWatch(void* pool) {
    gHvAlertPool = pool;
    gHvWatch.clear();
    gHvGone.clear();
    gHvSeeded = false;
}

void PushGone(int dropId, const HighValueWatch& w) {
    if (!w.published) return;
    const DWORD now = GetTickCount();
    if (w.publishedMs != 0 &&
        static_cast<int>(now - w.publishedMs) > static_cast<int>(kHvPickSuccessMaxAgeMs))
        return;
    if (gHvGone.size() >= kHvGoneCap) return;
    HighValueDropAlert a{};
    a.dropId = dropId;
    a.itemId = w.itemId;
    a.kind = w.kind;
    gHvGone.push_back(a);
}

void FillWatchBag(HighValueWatch& w, int itemId) {
    unsigned long long bag = 0;
    w.bagKnown = CountBagItem(itemId, bag);
    w.bagAtPublish = w.bagKnown ? bag : 0;
}

void BumpBagBaseline(int itemId, unsigned long long bagNow) {
    if (itemId <= 0) return;
    for (auto& kv : gHvWatch) {
        if (kv.second.itemId != itemId) continue;
        if (!kv.second.bagKnown) continue;
        if (kv.second.bagAtPublish < bagNow)
            kv.second.bagAtPublish = bagNow;
    }
}

// 池里暂时看不到：要消耗栏数量增加才报成功。iterator 被 erase 时返回 true。
bool FinishMissingWatch(std::unordered_map<int, HighValueWatch>::iterator& it, DWORD now) {
    HighValueWatch& w = it->second;
    const DWORD goneMs = now - w.missingSince;
    if (goneMs < kHvGoneDebounceMs) return false;
    unsigned long long bagNow = 0;
    const bool bagOk = CountBagItem(w.itemId, bagNow);
    if (w.bagKnown && bagOk && bagNow > w.bagAtPublish) {
        const int dropId = it->first;
        const int itemId = w.itemId;
        const unsigned long long bagWas = w.bagAtPublish;
        PushGone(dropId, w);
        BumpBagBaseline(itemId, bagNow);
        it = gHvWatch.erase(it);
        x::runtime::LogI("droppool", "hvPick bag dropId=%d itemId=%d %llu→%llu", dropId, itemId,
                         bagWas, bagNow);
        return true;
    }
    if (goneMs >= kHvGoneGiveUpMs) {
        x::runtime::LogI("droppool", "hvPick silent dropId=%d itemId=%d bag=%llu goneMs=%u",
                         it->first, w.itemId, bagNow, (unsigned)goneMs);
        it = gHvWatch.erase(it);
        return true;
    }
    return false;
}
}  // namespace

int CollectNewHighValueDropAlerts(HighValueDropAlert* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;
    if (!EnsureBound()) return 0;
    void* pool = PeekDropPool();
    if (!LooksLikeHeapPtr(pool)) pool = ResolveDropPool(GetTickCount());
    if (!LooksLikeHeapPtr(pool)) return 0;

    if (pool != gHvAlertPool) {
        ResetHighValueWatch(pool);  // 换池先静默登记现有件，避免进图瞬间刷一串提示
    }

    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    // 池内仍在的 id：顺带修剪已吸走的，避免 set 无限涨
    std::unordered_set<int> live;
    live.reserve(static_cast<size_t>(count) + 8u);

    int nOut = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        const int id = ReadI32(drop, kOffDropId);
        if (id == 0) continue;
        live.insert(id);
        if (ReadU8(drop, kOffDropIsMoney) != 0) continue;
        const int info = ReadI32(drop, kOffDropInfo);
        const HvClass hv = ClassifyHighValueItem(info, false);
        if (hv != HvClass::Scroll && hv != HvClass::Dart) continue;  // 卷軸 / 雷之鏢；其它装备不提醒
        if (!DropClientPickable(drop)) continue;
        if (!gHvSeeded) {
            HighValueWatch w{};
            w.itemId = info;
            w.kind = static_cast<int>(hv);
            FillWatchBag(w, info);
            gHvWatch.emplace(id, w);
            continue;
        }
        if (gHvWatch.contains(id)) continue;
        // 本拍 out 已满：不要 insert，留给下一拍再吐，避免永久漏提示
        if (nOut >= maxOut) continue;
        HighValueWatch w{};
        w.itemId = info;
        w.kind = static_cast<int>(hv);
        w.published = true;
        w.publishedMs = GetTickCount();
        FillWatchBag(w, info);
        gHvWatch.emplace(id, w);
        out[nOut].dropId = id;
        out[nOut].itemId = info;
        out[nOut].kind = static_cast<int>(hv);
        ++nOut;
    }
    if (!gHvSeeded) gHvSeeded = true;
    const DWORD now = GetTickCount();
    for (auto it = gHvWatch.begin(); it != gHvWatch.end();) {
        if (live.contains(it->first)) {
            if (it->second.missingSince != 0) {
                const DWORD goneMs = now - it->second.missingSince;
                if (goneMs >= kHvGoneDebounceMs)
                    x::runtime::LogI("droppool", "hvPick revive dropId=%d itemId=%d goneMs=%u",
                                     it->first, it->second.itemId, (unsigned)goneMs);
                it->second.missingSince = 0;
            }
            ++it;
            continue;
        }
        // 进图静默登记的件：从池消失不报成功
        if (!it->second.published) {
            it = gHvWatch.erase(it);
            continue;
        }
        if (it->second.missingSince == 0) it->second.missingSince = now;
        if (FinishMissingWatch(it, now)) continue;
        ++it;
    }
    return nOut;
}

int CollectGoneHighValueDrops(HighValueDropAlert* out, int maxOut) {
    if (!out || maxOut <= 0) return 0;
    int n = 0;
    while (n < maxOut && !gHvGone.empty()) {
        out[n++] = gHvGone.front();
        gHvGone.erase(gHvGone.begin());
    }
    return n;
}

bool TryFootPickup(FootResult& out) {
    out = {};
    if (!EnsureBound()) {
        out.why = "unbound";
        return false;
    }

    gFoot.result = {};
    gFoot.done = false;

    if (!x::runtime::main_thread::Ensure()) {
        out.why = "no_pump";
        return false;
    }
    if (!x::runtime::main_thread::InvokeAndWait(&FootJobThunk, nullptr, kJobWaitMs,
                                               x::runtime::main_thread::JobPrio::Low)) {
        out.why = "timeout";
        return false;
    }
    out = gFoot.result;
    return out.ok;
}

bool TryCharVacuum(float halfW, float halfH, int maxSend, const SkipIds* skipIds,
                   CharVacResult& out) {
    out = {};
    if (!EnsureBound()) {
        out.why = "unbound";
        return false;
    }
    if (!(halfW > 1.f) || !(halfH > 1.f)) {
        out.why = "bad_box";
        return false;
    }

    gCharVac.halfW = halfW;
    gCharVac.halfH = halfH;
    gCharVac.maxSend = maxSend > 0 ? maxSend : 1;
    gCharVac.skip = {};
    if (skipIds) gCharVac.skip = *skipIds;
    PublishLiveSkip(skipIds);
    EnsureTryPickProbe();
    gCharVac.result = {};
    gCharVac.done = false;

    if (!x::runtime::main_thread::Ensure()) {
        out.why = "no_pump";
        return false;
    }
    if (!x::runtime::main_thread::InvokeAndWait(&CharVacJobThunk, nullptr, kJobWaitMs,
                                               x::runtime::main_thread::JobPrio::Low)) {
        out.why = "timeout";
        return false;
    }
    out = gCharVac.result;
    return out.ok;
}

}  // namespace x::features::ports::drop
