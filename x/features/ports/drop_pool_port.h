#pragma once
// drop_pool_port — Classic TWMS DropPool 只读 + 宠物吸物 + 角色脚边拾取
// 真源：docs/features/pet_loot/P0a_锚点复核.md
// 宠吸：.rdata 矩形包 → Pet.TryPickUpDrop → ByPet → Pet.Send（禁止手组包 / 改 GA .text）
// 变态宠吸：启用时写一次 .rdata 真空尺寸，关掉还原原生 50×60；不投泵调 ByPet
// 脚下：只自动触发原生 DropPool.TryPickUpDrop(userPos)；不盖戳、不清闸、不扩盒

#include <cstdint>
#include <unordered_set>

namespace x::features::ports::drop {

constexpr int kPetSkillPickupItem = 1;
constexpr int kPetSkillLongRange = 2;
constexpr int kPetSkillDropSweep = 4;
constexpr int kPetSkillPickUpAll = 16;
constexpr int kMesoSkipId = 2147483647;  // 与官方金币例外占位一致

struct Rect4 {
    float x = 0.f;
    float y = 0.f;
    float w = 0.f;
    float h = 0.f;
};

struct DropLite {
    void* ptr = nullptr;
    int id = 0;
    int info = 0;
    bool isMoney = false;
    float x = 0.f;
    float y = 0.f;
};

struct ProbeSnapshot {
    bool ok = false;
    bool hasPet = false;
    void* pet = nullptr;
    int dropCount = 0;
    int nearCount = 0;
    uint16_t petSkill = 0;
    Rect4 petRc{};
    Rect4 collisionRcPet{};
    float petX = 0.f;
    float petY = 0.f;
    // 最近一件 Drop.Pt1（诊断 near=0：对照宠 Ap）
    bool hasSampleDrop = false;
    float sampleDropX = 0.f;
    float sampleDropY = 0.f;
    float sampleDropDist = -1.f;
};

struct VacuumResult {
    bool called = false;
    bool ok = false;
    const char* why = "idle";
    int dropCount = 0;
    int dropCountAfter = 0;
    int nearCount = 0;
    int dropsDelta = 0;
    int gatesCleared = 0;  // 拍前清 LastTry/EndParabolic 个数（宠吸现多为 0）
    int flyHeld = 0;       // ByPet 前临时挡住的未落地件数（LastTry 戳，拍末还原）
    int skipStamped = 0;   // 黑名单 LastTry=INT_MAX 盖戳数（宠吸主路径）
    int ownSkipped = 0;    // 盒内非己/非无主（客户端预筛跳过）
    int sampleOwnType = -1;  // Drop.OwnType（0..4；未解析=-1）
    int sampleOwnRaw = 0;    // 死钉 +0x3C 原始 i32（诊断）
    int sampleOwnerId = 0;   // Drop.OwnerId@+0x34（归属预筛）
    int localCharId = 0;     // 掉落侧自己 OwnerId（认亲后）；未认亲为 0
    int remoteUsers = -1;    // UserPool 远程人数；-1=Peek 失败（勿当独图）
    int sampleLastTry = 0;
    int sampleEndPara = 0;
    // 金币诊断（CountDropsNear 顺带读，不加第三趟扫池）
    int nearMoney = 0;       // 真空盒内 IsMoney 数
    int nearItem = 0;        // 真空盒内非金币数
    int sampleIsMoney = -1;  // -1 无样本 / 0 道具 / 1 金币（优先采金币）
    int sampleInfo = 0;      // Drop.Info（非金多为 itemId）
    // 拍后轻扫：ByPet 成功 Send 会盖 LastTry/PickStamp；池未掉 → 疑似服拒/满栏占坑
    int sendTouch = 0;        // 盒内盖戳件数（拍前已清闸；排除自家退避/黑名单戳）
    int sendTouchMoney = 0;   // 其中 IsMoney
    int sentButPoolSame = 0;  // 有提交迹象且本拍池未掉
    // 退避黑名单：ByPet 每拍只提交一件，被拒的那件会堵死队头（某栏满时最明显）
    int stallStamped = 0;   // 本拍盖住的退避中件数
    int stallRestored = 0;  // 拍末还原数；应等于 stallStamped，不等即有残留戳
    int stallHeld = 0;      // 退避集合内仍生效的 dropId 数
    // 高价值优先（装备/卷軸）
    int highValueNear = 0;     // 盒内可吸高价值（对应栏有空位）
    int highValueSkippedFull = 0;  // 高价值但栏满跳过
    bool highValueUrgent = false;  // 有可吸高价值 → 应打断出刀
    int highValueSampleDropId = 0;  // 扫盒样本 dropId（Peek/Scan 首件）
    int highValueSampleInfo = 0;    // 扫盒样本 itemId
    int highValueSampleKind = 0;    // 0无 1装备 2卷軸
    int pacedPickDropId = 0;        // 本拍按件 Send 选中的 dropId
    int pacedPickInfo = 0;          // 选中 itemId（金币时为金额）
    int pacedPickRank = 0;          // 0普通 1金币 2高价值
    // 服端异步清池：同拍 Δ 常为 0；跨拍 dropCount < 上拍 after → 真吸
    bool poolFellSinceLast = false;
    uint16_t petSkill = 0;       // GetUpgradePetSkill()（= GetItemSlot→usPetSkill）
    uint16_t petSkillSlot = 0;   // 直读 ItemSlotPet.usPetSkill@+0x3C（ByPet 真源路径）
    Rect4 beforeRc{};
    Rect4 afterRc{};
    // ByPet→Send 多为直接 call，MI 探针可能恒 0；以 dropsΔ / poolFell 为准
    uint32_t petSendHits = 0;
    uint32_t poolSendHits = 0;
    uint32_t petSendDelta = 0;
    uint32_t poolSendDelta = 0;
};

struct FootResult {
    bool called = false;
    bool ok = false;
    const char* why = "idle";
    int dropCount = 0;
    int dropCountAfter = 0;
    int dropsDelta = 0;
    bool poolFellSinceLast = false;
    float userX = 0.f;
    float userY = 0.f;
    uint32_t petSendHits = 0;
    uint32_t poolSendHits = 0;
    uint32_t petSendDelta = 0;
    uint32_t poolSendDelta = 0;
};

// 人物直吸 = 宠吸同一套控制面，主体换成角色（不靠宠、不改 ByPet 矩形包）。
// 清闸 / 退避盖戳 / 黑名单盖戳 / 一调一件 / 拒收即 AddStall / 拍末还原 —— 与 RunVacuumOnMain 对齐。
struct CharVacResult {
    bool called = false;
    bool ok = false;
    const char* why = "idle";
    int dropCount = 0;
    int dropCountAfter = 0;
    int dropsDelta = 0;
    int nearCount = 0;
    int nearWant = 0;
    int sent = 0;             // 本拍送包数（对齐 ByPet：应为 0 或 1）
    int sentDropId = 0;       // 本拍送出的 dropId（Send 不写 LastTry 时靠它做退避）
    int gatesCleared = 0;
    int skipStamped = 0;
    int sendTouch = 0;
    int sentButPoolSame = 0;
    int stallStamped = 0;
    int stallRestored = 0;
    int stallHeld = 0;
    int gateBlocked = 0;      // Pickable=false 等读侧挡下
    bool poolFellSinceLast = false;
    float userX = 0.f;
    float userY = 0.f;
    uint32_t sentTotal = 0;
};

// 黑名单 itemId 集合：宽词（「卷」「色」）可达数千，禁止固定上限截断。
struct SkipIds {
    std::unordered_set<int> ids;

    void clear() { ids.clear(); }
    bool empty() const { return ids.empty(); }
    size_t size() const { return ids.size(); }
    bool contains(int id) const { return ids.find(id) != ids.end(); }
    bool insert(int id) {
        if (id <= 0) return false;
        return ids.insert(id).second;
    }
};

void Init();
void Shutdown();
bool EnsureBound();
void* PeekLocalUser();
void* PeekDropPool();

bool CollectProbe(ProbeSnapshot& out, float nearHalfW, float nearHalfH);

// highValuePriority：装备/卷軸优先；对应背包栏满则跳过该件；有可吸件时 out.highValueUrgent
bool TryPetVacuum(float vacuumW, float vacuumH, const SkipIds* skipIds, VacuumResult& out,
                  bool highValuePriority = false);

// 变态宠吸：常驻写入 ByPet .rdata（GUI 真空宽高）。尺寸未变则 no-op。
// 不调 TryPickUpDrop。失败（未定位矩形包 / VirtualProtect）返回 false。
bool HoldByPetRectPack(float vacuumW, float vacuumH);
// 还原原生 25/10 + 50×60 并恢复页保护。未 hold 则 no-op。
void ReleaseByPetRectPack();

// 出刀让路 / 堵泵时不调 ByPet，但仍盖黑名单戳（LastTry+EndPara）并尽量同步 ExceptionList。
// 纯内存读写，worker 可调。挡住原生脚边 50x60 在真空暂停时把箭矢舔走。
// 落地当帧：EnsureBound 会给 Pet.TryPickUpDrop 换 MI，原生宠 Tick 进钩后再盖戳（早于 E8 ByPet）。
// 人吸/脚下/拾物关闭同样要 PublishLiveSkip；原生宠不看面板档位。
// 返回本拍新盖戳数。
int HoldSkipDrops(const SkipIds* skipIds, float halfW, float halfH);

// 掉落落地加速（独立开关，默认关）。纯内存写，worker 可调。不改 GA .text。
// snapLand：EndPara∈{0,1,2} 且 PickPt 非 (0,0) → Pt1=PickPt、EndPara=3、LastTry=0。
//           必须跟位置；禁止只写 EndPara=3。禁写 EndPara=0。
// accelFall：同态加大 LastTry（t=LastTry/1000）；两勾都开时瞬落优先。
// skipIds：黑名单件禁止写成 Ready(3)，也禁止跟位置（跟位置会让游戏把态写回 3）。
//          只盖 SkipHold(4)+INT_MAX+Pickable=0。
//          itemId 尚未写出的非金币件也不瞬落/加速（避免未识别箭矢被捡）。
int BoostDropFall(bool snapLand, bool accelFall, const SkipIds* skipIds = nullptr,
                  int* outSnap = nullptr, int* outAccel = nullptr, int* outSkipHold = nullptr);

// 纯内存扫池：宠真空 ∩ 角色半盒内是否有「栏未满」的装备/卷軸（与 Send 同口径；worker 可调）
// 失败 / 无角色位 → false（调用方应 fail-closed 清 urgent）
// 可选 outSample*：首件可吸 HV 的 dropId/itemId/kind(1装备/2卷)
bool PeekHighValueActionable(float petX, float petY, float halfW, float halfH, const SkipIds* skip,
                             int& outNearHv, int& outSkippedFull, int* outSampleDropId = nullptr,
                             int* outSampleInfo = nullptr, int* outSampleKind = nullptr);

struct HighValueDropAlert {
    int dropId = 0;
    int itemId = 0;
    int kind = 0;  // 1=装备 2=卷軸 3=雷之鏢（与 ClassifyHighValueItem 一致）
};

// 雷之鏢 2070005：消耗栏飞镖；提醒 + 高价值优先吸。不是 1xxxxxx 装备，也不是 204 卷。
constexpr int kThunderDartItemId = 2070005;

// 全图扫池：新出现的可捡卷軸 / 雷之鏢（按 dropId 去重；换 DropPool 清空）。其它装备不进提示。
int CollectNewHighValueDropAlerts(HighValueDropAlert* out, int maxOut);

// 已叮咚过的卷軸/雷之鏢从池里消失，且消耗栏该 itemId 数量增加，才报拾取成功。
// 仅从池消失（空吸/闪漏/别人捡）不报；同一 dropId 再出现不重复叮咚。换池不清。
int CollectGoneHighValueDrops(HighValueDropAlert* out, int maxOut);

// 主线程：仅 DropPool.TryPickUpDrop(角色位)；范围/门禁全交给游戏原生
bool TryFootPickup(FootResult& out);

// 主线程：枚举池 → 复刻门禁 → DropPool.SendDropPickUpRequest(角色真实位置, dropId, 0)
// maxSend 建议 1（对齐 ByPet 一调一件）；吞吐靠外层 burst 连调，别在同一次 Invoke 里连发。
bool TryCharVacuum(float halfW, float halfH, int maxSend, const SkipIds* skipIds,
                   CharVacResult& out);

}  // namespace x::features::ports::drop
