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

#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "Psapi.lib")

namespace x::features::ports::drop {
namespace {

using x::runtime::il2cpp::ArrayAt;
using x::runtime::il2cpp::ArrayLen;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// Unity FindAll → x::runtime::il2cpp::kRvaFindObjectsOfTypeAll（il2cpp_bind.h SSOT）
// Remount 2026-08-06：方法 RVA 普遍 +0x1E70；字段 off 未漂；ACS 类/字段哈希全换
constexpr uint32_t kRvaPetTryPickUpDrop = 0xF9EA50;  // remounted 2026-08-06 TDI:1516
constexpr uint32_t kRvaDropTryPickUpDrop = 0xF59610;  // remounted 2026-08-06 DropPool.TryPickUpDrop(in Vector2)
constexpr uint32_t kRvaDropTryPickUpDropByPet = 0xF5B7F0;  // remounted 2026-08-06
constexpr uint32_t kRvaPetGetUpgradePetSkill = 0xF5ED80;  // remounted 2026-08-06
constexpr uint32_t kRvaPetGetItemSlot = 0xF5DBC0;  // remounted 2026-08-06 · ByPet → ItemSlotPet
constexpr uint32_t kRvaPetIsInExceptionList = 0xF53A90;  // remounted 2026-08-06
constexpr uint32_t kRvaPetSendDropPickUp = 0xF5DF00;  // remounted 2026-08-06 Pet.SendDropPickUpRequest
constexpr uint32_t kRvaPoolSendDropPickUp = 0xF5AF10;  // remounted 2026-08-06 DropPool.SendDropPickUpRequest
// ByPet Contains 真源（.rdata，非 CollisionCheck / _rcPet）：
//   int32 offX,offY @ +0 ; float w,h @ +0x10
//   rect = (petPos - (offX,offY), w, h)；原生 (25,10)+(50,60)
//   IDA：ByPet → psubd xmm9,[rip+disp] / movsd xmm0,[rip+disp]；旧死钉仅作末级兜底
constexpr uint32_t kRvaByPetRectPackFallback = 0x55832D0;  // remounted 2026-08-06 · runtime pattern bind
constexpr int32_t kNativeRectOffX = 25;
constexpr int32_t kNativeRectOffY = 10;
constexpr float kNativeRectW = 50.f;
constexpr float kNativeRectH = 60.f;
// ByPet 函数体扫描上限（当前 size≈0x22a6）
constexpr size_t kByPetScanMax = 0x2800;

constexpr char kHashPetTryPickUp[] =
    "bc8b8a65a5167d063b4d0ecd874e9b5b3cfd742f70f339e3a2aa468c13088ff";  // remounted 2026-08-06
constexpr char kHashDropTryPickUp[] =
    "bd44bfbfd1e3659fc6b1b9ad32347f88ca8368838477b55e0b73c8f2d405adf";  // remounted 2026-08-06
constexpr char kHashDropTryPickUpByPet[] =
    "d14c4caca4f411b2afbaaff44ad6e04d6678b84cf7da0011b798472d6ba585e";  // remounted 2026-08-06
constexpr char kHashPetGetUpgradeSkill[] =
    "abb76b81355615cded21efd89bdbc644f60b9e60d50f4eec97d6488c91417f5";  // remounted 2026-08-06
constexpr char kHashPetGetItemSlot[] =
    "a5a3afdba943de0eb68100717af64fdcf1cf85ce640a5b3d6f10ce97c9c8e34";  // remounted 2026-08-06
constexpr char kHashPetIsInException[] =
    "e8a049a7ba742c0777d4d948205695c3068a6ef58f55707414015904d0a95a1";  // remounted 2026-08-06
constexpr char kHashPetSendDropPickUp[] =
    "a6661ab1b790c8deb4cc26d667ab367d175ce45cc69c9c7a4318076a0105665";  // remounted 2026-08-06
constexpr char kHashPoolSendDropPickUp[] =
    "e928a49209b00a967822dfe207828bffd6ede1c4590c711f2e6c39e7cfeb327";  // remounted 2026-08-06

constexpr char kDropPoolClass[] =
    "c75c9e590ecff774417e635841d8b4c530112e2289e55763122899e775f045c";  // remounted 2026-08-06 TDI:1489
constexpr char kDropClass[] =
    "d20bf485543c953be99da021d3d6ead5488905d0a1791345812686ca0b23591";  // remounted 2026-08-06 TDI:1488
// UserLocal → il2cpp_shape::ResolveUserLocalKlass
constexpr char kCollisionCheckClass[] =
    "ce5571fdd447d9de7395a41a89268b50d757cf9aa1dd8b20f07ee1f691276f2";  // remounted 2026-08-06 TDI:2446
constexpr char kPetClass[] =
    "f170c222994b5b5fa20c2a3c92fa28d6d8f6812c6955b27c6141300e37a575d";  // remounted 2026-08-06 TDI:1516
constexpr char kUserClass[] =
    "b8c9aedb2c800fa8ec9515b0f728235725989303f6bb609bafebeee4a902078";  // TDI:1560 User（m_apPet/CurPos）
constexpr char kVecCtrlOwnerClass[] =
    "edc85ce203606bdb549e5fb94458b1d2d11ce78034d24d41e39a54c0288d38e";  // TDI:1586
constexpr char kVecCtrlClass[] =
    "e0eb55b82f10cb9eeb9424eb3aadf1450a014afa564bc55c3739b2909abfbbc";
constexpr char kItemSlotPetClass[] =
    "bb1b627de814571bee19f0047969130afb63cd6cb00856136624230a9fbc30e";  // GetItemSlot 返回类型

// —— 字段防漂移：hash + field_get_offset；下列常量仅 dump 验证 fallback（off 未漂）——
constexpr size_t kFbApPet = 0x2B0;
constexpr size_t kFbWmMyUser = 0x28;
constexpr size_t kFbPoolDict = 0x20;
constexpr size_t kFbPetRc = 0x100;
constexpr size_t kFbPetExceptionList = 0x90;
constexpr size_t kFbVecCtrl = 0x50;
constexpr size_t kFbFieldPos = 0x64;
constexpr size_t kFbCurPos = 0x240;
constexpr size_t kFbVcAp = 0x98;  // AbsPos 结构起点；Y = Ap+8
constexpr size_t kFbDropId = 0x30;
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
    "b093d698bf1ad623c0b88a522673c36b6b8aa11110ccdcb5e36472da2e50906";
constexpr char kHashFldWmMyUser[] =
    "<ef4652eaf850c6fcba53fee8385959f223111ee46e00417bb435eebc026d15e>k__BackingField";  // WM.MyUser@0x28
constexpr char kHashFldPoolDict[] =
    "f11e3abd2d55a7e7efeaf7787be0f3439360df65142fe78fad2455dd74ee2a5";
constexpr char kHashFldPetRc[] =
    "e3f2d91c794cf4b07c22c83bfca96cff84ae49d0f2b52637b1a07121e5e57cb";
constexpr char kHashFldPetExceptionList[] =
    "<ed9b360d6618f5c62618c05bf7e79921c0db4127eb748a4fa443ab619fb2e01>k__BackingField";
constexpr char kHashFldVecCtrl[] =
    "<acb8946a384ed398c4ad9268349397cf4f6e65cf136078ebc9aa26a949efd41>k__BackingField";
constexpr char kHashFldFieldPos[] =
    "cc96f38a9acbe6b4e8005a2d56a7846324bc67690c2059661962502f74b928a";
constexpr char kHashFldCurPos[] =
    "c4adef19821f3737cd477a7840968c11697f4afd8eb8696cafb37d1c297b926";
constexpr char kHashFldVcAp[] =
    "e558fbd3da65bf13bea9360dfa61506af709ad89f925bc16b67e7e1cdb24107";
constexpr char kHashFldDropId[] =
    "cd0df968addeeaf2958ad3cf9d93cfbfad8bbfe1887852bed9220643674cac1";
constexpr char kHashFldDropOwnType[] =
    "da3077a0c8458d43058c7356cb5a42132804698cdabe3f969fca5337cb5fa68";
constexpr char kHashFldDropIsMoney[] =
    "d49606e6e2484b745ce3b360bfc5ec8e925772cd0c17785ad9f5ecbf141141a";
constexpr char kHashFldDropInfo[] =
    "d857faa67a6048709e02d88aeafd4e1890ddbb150e2972dd5d985449f8617c1";
constexpr char kHashFldDropPt1[] =
    "a400df6c99ccac1ffa49ee8f242be7a0301f114f47c5af30069cbf0c9325efe";
constexpr char kHashFldDropEndPara[] =
    "a91a5f1789db1b1ce16909bb4a443f800d455f0dac3445ed44889d70ba60be9";
constexpr char kHashFldDropLastTry[] =
    "bc43d5785808b3ede93ae6e2379f116edf30bd0d65a4b102ebc0f32a1a0eb2c";
constexpr char kHashFldDropPickStamp[] =
    "e381c2c7599f152c899872a0af9b13b7066a6774a185422501ebf26c36f32ac";
constexpr char kHashFldDropPickPt[] =
    "f3710236c8f81a1d0353950d0d09c6c6134752ffdb6d07232d8985a30efaec0";
constexpr char kHashFldDropPickable[] =
    "ea772e389eb98128fa6a41d6d1236265341d8538dca1d9b5f46874d5c67a311";
constexpr char kHashFldItemSlotPetSkill[] =
    "a062fd375c28f78b5e031162b3955bc9d80395e0d9c3e3460b3ac6aefebd878";
constexpr char kHashFldCollisionRcPet[] =
    "c9e0b4a2713b66aab3eb48f6cbcc87123ac8b666d485d5a35037130856f842f";

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
// IDA ByPet：cmp [drop+0x7C], 3 / cmovz 才继续；写 0 会重置抛物线（近图飞落根因）
constexpr int kEndParaReady = 3;
// 黑名单挡 ByPet：必须 !=3；禁止写 0（会重置抛物线飞落）
constexpr int kEndParaSkipHold = 4;
constexpr int kLastTrySkipStamp = 0x7FFFFFFF;

// 人物直吸（不靠宠）：复刻官方 DropPool.TryPickUpDrop（RVA 0xF59610）的门禁语义，然后直接调
// 它自己的 DropPool.SendDropPickUpRequest（RVA 0xF5AF10）。控制面与宠吸同构（清闸/退避盖戳/
// 黑名单盖戳/拒收即 AddStall/拍末还原），只是中心从宠坐标换成角色坐标、送包走人物入口。
// 官方那条链逐指令实读所得：
//   ① [drop+0x2D] 必须为真                       ② EndPara(0x7C) == 3
//      （IMM 0x328634BB + seed@0x7FFB8A2C92A8=0xCD79CB48 → 3）
//   ③ now - PickStamp(0x88) >= 3000（有符号）——宠吸同款拍前清闸后由官方 Send 再盖
//   ④ 归属链交由服务端裁决 + 退避兜底
//   ⑤ 矩形：宠吸扩 ByPet 包；人物直吸用 vacuum 半盒枚举（与宠吸共用 vacuumW/H，默认 300×200）
//      —— 送包坐标仍是角色位；过大盒会多打拒收，靠 pending/AddStall 与 burst=1 兜底
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
// ByPet .rdata 矩形包：xref/特征定位缓存（拍内 patch 前必须已 resolve）
uint8_t* gByPetRectPack = nullptr;
const char* gByPetRectVia = nullptr;
std::atomic<uint32_t> gPetSendHits{0};
std::atomic<uint32_t> gPoolSendHits{0};
std::atomic<bool> gSendProbeInstalled{false};
// 拒收连击整段休眠截止 tick（worker 侧可读，避免退避期仍排队 MainPump）
std::atomic<DWORD> gRejectBackoffUntil{0};

DWORD gLastLuRebind = 0;
DWORD gLastPoolRebind = 0;

struct VacJob {
    float vacuumW = 300.f;
    float vacuumH = 200.f;
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

void WriteI32(void* obj, size_t off, int32_t v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
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
    const bool okOwn =
        hit(FieldOffOrFb(dropKlass, kHashFldDropOwnType, kFbDropOwnType, &gOff.dropOwnType, false));
    hit(FieldOffOrFb(dropKlass, kHashFldDropIsMoney, kFbDropIsMoney, &gOff.dropIsMoney, false));
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
    *outOrig = mi->methodPointer;
    mi->methodPointer = hook;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return true;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    mi->methodPointer = orig;
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

bool __fastcall HookPetSend(void* self, uint64_t ptPacked, int dropId, uint32_t crc1, uint32_t crc2,
                            const void* methodInfo) {
    gPetSendHits.fetch_add(1, std::memory_order_relaxed);
    auto* fn = reinterpret_cast<FnPetSend>(gOrigPetSend);
    return fn ? fn(self, ptPacked, dropId, crc1, crc2, methodInfo) : false;
}

void __fastcall HookPoolSend(void* self, const void* ptIn, int dropId, uint32_t crc,
                             const void* methodInfo) {
    gPoolSendHits.fetch_add(1, std::memory_order_relaxed);
    auto* fn = reinterpret_cast<FnPoolSend>(gOrigPoolSend);
    if (fn) fn(self, ptIn, dropId, crc, methodInfo);
}

void EnsureSendProbe() {
    if (gSendProbeInstalled.load(std::memory_order_acquire)) return;
    if (!gGA || !gClassGetMethods) return;
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
            PatchMethodInfo(gMiPoolSend, reinterpret_cast<void*>(&HookPoolSend), &gOrigPoolSend)) {
            okPool = true;
        }
    } else if (gMiPoolSend && gOrigPoolSend) {
        okPool = true;
    }
    if (okPet || okPool) {
        gSendProbeInstalled.store(true, std::memory_order_release);
        x::runtime::LogI("DropPort",
                         "SendProbe MI pet=%d pool=%d (direct-call ByPet 可能不经 MI；以 dropsΔ 为准)",
                         okPet ? 1 : 0, okPool ? 1 : 0);
    }
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
    if (!pool || dropId == 0) return false;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return false;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return false;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return false;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        if (ReadI32(drop, kOffDropId) == dropId) return true;
    }
    return false;
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
            gLastLuRebind = 0;  // 强制 fall-through 立刻重绑
        } else if (!LooksLikeHeapPtr(myUser)) {
            // 换图空窗 MyUser 暂空：丢掉旧缓存，勿继续当活的用。
            gLocalUser = nullptr;
            gLastLuRebind = 0;
        } else if (LooksLikeHeapPtr(gLocalUser) && ReadPtr(gLocalUser, 0) &&
                   ReadPtr(gLocalUser, 0x10)) {
            MaybeClearLoginFreeze();
            return true;
        } else {
            gLocalUser = nullptr;
        }
    }
    if (gLastLuRebind && now - gLastLuRebind < kRebindMs && !gLocalUser) return false;
    gLastLuRebind = now;
    gLocalUser = nullptr;
    if (!BindIl2Cpp()) return false;

    void* wm = world::PeekWorldManager();
    if (!wm) wm = world::GetWorldManager();
    void* myUser = ReadPtr(wm, kOffWmMyUser);
    // 禁 GetGoName（worker）；WM.MyUser + m_CachedPtr 即权威。
    if (UnityObjectAlive(myUser)) {
        gLocalUser = myUser;
        x::runtime::LogI("DropPort", "LocalUser ACCEPT wm.MyUser=%p", gLocalUser);
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
        gLocalUser = cand;
        x::runtime::LogI("DropPort", "LocalUser ACCEPT FindAll=%p", gLocalUser);
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
        x::runtime::LogI("DropPort", "DropPool bind %p score=%d", gDropPool, bestScore);
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
    const int own = ReadI32(drop, kOffDropOwnType);
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
    if (own < 0 || own > 4) {
        WriteI32(drop, kOffDropOwnType, kDropOwnNo);
        touched = true;
    }
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
            if (outSampleOwn) *outSampleOwn = ReadI32(drop, kOffDropOwnType);
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
    int sampleIsMoney = -1;
    int sampleInfo = 0;
    int sampleOwn = -1;
    int sampleLast = 0;
    int sampleEnd = 0;
    int gatesCleared = 0;
    int stallStamped = 0;
    int skipStamped = 0;
};

PetVacNearPass PreparePetVacNearPass(void* pool, float cx, float cy, float halfW, float halfH,
                                     DWORD now, const SkipIds* skip, int maxClear) {
    PetVacNearPass out{};
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
            if (writesOk) {
                const int last = ReadI32(drop, kOffDropLastTry);
                const int endp = ReadI32(drop, kOffDropEndPara);
                bool touched = false;
                if (last != kLastTrySkipStamp) {
                    WriteI32(drop, kOffDropLastTry, kLastTrySkipStamp);
                    touched = true;
                }
                if (endp == kEndParaReady) {
                    WriteI32(drop, kOffDropEndPara, kEndParaSkipHold);
                    touched = true;
                }
                if (touched) ++out.skipStamped;
            }
            continue;
        }

        const int lastTry = ReadI32(drop, kOffDropLastTry);
        const int endPara = ReadI32(drop, kOffDropEndPara);
        if (!gateSampled) {
            gateSampled = true;
            out.sampleOwn = ReadI32(drop, kOffDropOwnType);
            out.sampleLast = lastTry;
            out.sampleEnd = endPara;
        }

        // 可被 ByPet 真正捡的：已落地 + Pickable + 非退避戳
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

// ByPet 前软挡未落地：完整类型须在 RunVacuumOnMain 之前（栈数组）。
struct FlyHold {
    int id = 0;
    int prevLast = 0;
};
int StampNonReadyOutNear(void* pool, float cx, float cy, float halfW, float halfH,
                         const SkipIds* skip, FlyHold* out, int cap, int* outN);
int RestoreNonReadyOutNear(void* pool, float cx, float cy, float halfW, float halfH,
                           const FlyHold* holds, int holdN);

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
        if (!gJob.skip.empty() && pet) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);
        const DWORD nowTick = GetTickCount();
        const bool poolFell = PetVacPoolFellSinceLast(
            s_prevOk, s_prevTick, s_prevDropAfter, s_prevNear, s_prevGates, nowTick, r.dropCount);
        finishEmptyLike(nowTick, poolFell);
        return;
    }

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
    r.sampleLastTry = pass.sampleLast;
    r.sampleEndPara = pass.sampleEnd;
    r.gatesCleared = pass.gatesCleared;
    r.stallStamped = pass.stallStamped;
    r.skipStamped = pass.skipStamped;

    if (r.nearCount == 0) {
        if (!gJob.skip.empty()) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);
        const DWORD nowTick = GetTickCount();
        const bool poolFell = PetVacPoolFellSinceLast(
            s_prevOk, s_prevTick, s_prevDropAfter, s_prevNear, s_prevGates, nowTick, r.dropCount);
        finishEmptyLike(nowTick, poolFell);
        return;
    }

    // 盒内只有抛物中/不可捡：禁止调 ByPet。大盒下 ByPet 仍会碰未落地物盖戳，落地动画打转
    // （BIN：near>0 endPara=0/1 sendTouch>0 gates=0）。
    if (pass.readyNear <= 0) {
        if (!gJob.skip.empty()) (void)EnsureExceptionIdsIfNeeded(pet, gJob.skip);
        if (pool && r.stallStamped > 0) {
            r.stallRestored = RestoreStalledStampsNear(pool, px, py, halfW, halfH, now, skipPtr);
        }
        r.stallHeld = StallActiveCount(now);
        r.dropCountAfter = r.dropCount;
        r.dropsDelta = 0;
        r.called = false;
        r.ok = true;
        r.why = "wait_land";
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

    EnsureSendProbe();  // 仅即将调 ByPet 时装探针（空拍/无技能不再碰）

    // ByPet 前挡未落地：LastTry=INT_MAX（不写 EndPara）；拍末按 id 还原。
    FlyHold flyHolds[64]{};
    int flyN = 0;
    r.flyHeld = StampNonReadyOutNear(pool, px, py, halfW, halfH, skipPtr, flyHolds, 64, &flyN);

    ByPetRectBackup rectBak{};
    const bool rectPatched = PatchByPetRectPack(gJob.vacuumW, gJob.vacuumH, rectBak);

    if (!gMiTryPickUp && gPetKlass) {
        using x::runtime::il2cpp_method::MethodShape;
        using x::runtime::il2cpp_method::TypeKind;
        constexpr MethodShape kTry{0, TypeKind::Void, true, false, {}};
        gMiTryPickUp = ResolveMi(gPetKlass, kRvaPetTryPickUpDrop, kTry, "TryPickUpDrop",
                                 kHashPetTryPickUp);
    }
    auto fn = FnFromMi<FnPetTryPickUp>(gMiTryPickUp, kRvaPetTryPickUpDrop);
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
    if (pool && flyN > 0) {
        (void)RestoreNonReadyOutNear(pool, px, py, halfW, halfH, flyHolds, flyN);
    }

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
    }

    // 拍末必须把退避盖戳还原（含 seh / no_rect_patch 路径），否则切到脚边拾取时这些道具捡不起来
    if (pool && r.stallStamped > 0) {
        r.stallRestored = RestoreStalledStampsNear(pool, px, py, halfW, halfH, now, skipPtr);
    }
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
        } else if (r.sentButPoolSame) {
            if (s_rejectStrikes < 100) ++s_rejectStrikes;
            if (s_rejectStrikes >= kRejectBackoffNeed) {
                s_rejectBackoffUntil = nowTick + kRejectBackoffMs;
                gRejectBackoffUntil.store(s_rejectBackoffUntil, std::memory_order_relaxed);
                s_rejectStrikes = 0;
                x::runtime::LogW("droppool",
                                 "reject_backoff %ums (sentSame streak → skip vac; 清背包栏后自愈)",
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
    const bool writesOk = DropWritesAllowed();
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
            if (!writesOk) continue;
            const int last = ReadI32(drop, kOffDropLastTry);
            const int endp = ReadI32(drop, kOffDropEndPara);
            bool touched = false;
            if (last != kLastTrySkipStamp) {
                WriteI32(drop, kOffDropLastTry, kLastTrySkipStamp);
                touched = true;
            }
            // Ready(3) 会被 ByPet 吸入动画；改成 SkipHold，且绝不写 0
            if (endp == kEndParaReady) {
                WriteI32(drop, kOffDropEndPara, kEndParaSkipHold);
                touched = true;
            }
            if (touched) ++stamped;
            continue;
        }
        ++want;
    }
    if (outNear) *outNear = nearN;
    if (outWant) *outWant = want;
    if (outTotal) *outTotal = total;
    return stamped;
}

// ByPet 前把未落地挡出队头：只盖 LastTry=INT_MAX（绝不写 EndPara）。
// 官方 Contains 之后仍可能先碰抛物中物盖戳 → 落地打转；与 Stall 同款软挡。
// 记录 prevLast，拍末按 id 还原（不与 Stall/黑名单长期戳混淆：只还原本拍写下的 id）。
int StampNonReadyOutNear(void* pool, float cx, float cy, float halfW, float halfH,
                         const SkipIds* skip, FlyHold* out, int cap, int* outN) {
    if (outN) *outN = 0;
    if (!pool || !out || cap <= 0 || !DropWritesAllowed()) return 0;
    void* dict = ReadPtr(pool, kOffPoolDict);
    if (!LooksLikeHeapPtr(dict)) return 0;
    void* entries = ReadPtr(dict, kOffDictEntries);
    const int count = ReadI32(dict, kOffDictCount);
    if (!LooksLikeHeapPtr(entries) || count < 0 || count > 4096) return 0;
    const uintptr_t arrLen = ArrayLen(entries);
    if (arrLen == 0 || arrLen > 8192) return 0;

    int stamped = 0;
    int n = 0;
    for (uintptr_t i = 0; i < arrLen; ++i) {
        uint8_t* entry = x::runtime::il2cpp_container::DictEntryAt(entries, i, kEntrySize);
        if (ReadI32(entry, kOffEntryHash) < 0) continue;
        void* drop = ReadPtr(entry, kOffEntryValue);
        if (!LooksLikeHeapPtr(drop)) continue;
        float dpx = 0.f, dpy = 0.f;
        if (!ReadDropPt(drop, dpx, dpy)) continue;
        if (std::fabs(dpx - cx) > halfW || std::fabs(dpy - cy) > halfH) continue;
        if (skip && !skip->empty() && DropMatchesSkip(drop, *skip)) continue;

        const int endp = ReadI32(drop, kOffDropEndPara);
        if (endp == kEndParaReady) continue;  // 已落地：留给 ByPet

        const int last = ReadI32(drop, kOffDropLastTry);
        if (last == kLastTrySkipStamp) continue;  // 已是 Stall/黑名单戳

        const int id = ReadI32(drop, kOffDropId);
        if (id == 0) continue;

        WriteI32(drop, kOffDropLastTry, kLastTrySkipStamp);
        ++stamped;
        if (n < cap) {
            out[n].id = id;
            out[n].prevLast = last;
            ++n;
        }
    }
    if (outN) *outN = n;
    return stamped;
}

int RestoreNonReadyOutNear(void* pool, float cx, float cy, float halfW, float halfH,
                           const FlyHold* holds, int holdN) {
    if (!pool || !holds || holdN <= 0) return 0;
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
        float dpx = 0.f, dpy = 0.f;
        if (!ReadDropPt(drop, dpx, dpy)) continue;
        if (std::fabs(dpx - cx) > halfW || std::fabs(dpy - cy) > halfH) continue;
        if (ReadI32(drop, kOffDropLastTry) != kLastTrySkipStamp) continue;

        const int id = ReadI32(drop, kOffDropId);
        if (id == 0) continue;
        for (int h = 0; h < holdN; ++h) {
            if (holds[h].id != id) continue;
            WriteI32(drop, kOffDropLastTry, holds[h].prevLast);
            ++restored;
            break;
        }
    }
    return restored;
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

    const float halfW = gCharVac.halfW;
    const float halfH = gCharVac.halfH;
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

        __try {
            fn(pool, pt, bestId, 0u, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            seh = true;
        }
        if (!seh) {
            ++r.sent;
            r.sentDropId = bestId;
        }
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
    if (gMiPetSend && gOrigPetSend) RestoreMethodInfo(gMiPetSend, gOrigPetSend);
    if (gMiPoolSend && gOrigPoolSend) RestoreMethodInfo(gMiPoolSend, gOrigPoolSend);
    gMiPetSend = nullptr;
    gMiPoolSend = nullptr;
    gOrigPetSend = nullptr;
    gOrigPoolSend = nullptr;
    gSendProbeInstalled.store(false, std::memory_order_release);
    gDropPool = nullptr;
    gLocalUser = nullptr;
}

bool EnsureBound() {
    if (!BindIl2Cpp()) return false;
    const DWORD now = GetTickCount();
    const bool lu = ResolveLocalUser(now);
    void* pool = ResolveDropPool(now);
    if (!gPetKlass) gPetKlass = FindClass(kPetClass);
    if (lu && pool) EnsureSendProbe();
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

bool TryPetVacuum(float vacuumW, float vacuumH, const SkipIds* skipIds, VacuumResult& out) {
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
    gJob.skip = {};
    if (skipIds) gJob.skip = *skipIds;
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
