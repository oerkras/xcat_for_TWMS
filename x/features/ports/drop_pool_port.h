#pragma once
// drop_pool_port — Classic TWMS DropPool 只读 + 宠物吸物 + 角色脚边拾取
// 真源：docs/features/pet_loot/P0a_锚点复核.md
// 宠吸：.rdata 矩形包 → Pet.TryPickUpDrop → ByPet → Pet.Send（禁止手组包 / 改 GA .text）
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
    int sampleOwnType = -1;
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

bool TryPetVacuum(float vacuumW, float vacuumH, const SkipIds* skipIds, VacuumResult& out);

// 主线程：仅 DropPool.TryPickUpDrop(角色位)；范围/门禁全交给游戏原生
bool TryFootPickup(FootResult& out);

// 主线程：枚举池 → 复刻门禁 → DropPool.SendDropPickUpRequest(角色真实位置, dropId, 0)
// maxSend 建议 1（对齐 ByPet 一调一件）；吞吐靠外层 burst 连调，别在同一次 Invoke 里连发。
bool TryCharVacuum(float halfW, float halfH, int maxSend, const SkipIds* skipIds,
                   CharVacResult& out);

}  // namespace x::features::ports::drop
