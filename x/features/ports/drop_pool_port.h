#pragma once
// drop_pool_port — Classic TWMS DropPool 只读 + 宠物吸物 + 角色脚边拾取
// 真源：docs/features/pet_loot/P0a_锚点复核.md
// 正式吸物：.rdata 矩形包 → Pet.TryPickUpDrop → ByPet → Pet.Send（禁止手组包 / 改 GA .text）

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
};

struct VacuumResult {
    bool called = false;
    bool ok = false;
    const char* why = "idle";
    int dropCount = 0;
    int dropCountAfter = 0;
    int nearCount = 0;
    int dropsDelta = 0;
    int gatesCleared = 0;  // 拍前清 LastTry/EndParabolic 个数
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
    int sendTouch = 0;        // 盒内盖戳件数（拍前已清闸）
    int sendTouchMoney = 0;   // 其中 IsMoney
    int sentButPoolSame = 0;  // 有提交迹象且本拍池未掉
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
    int nearCount = 0;
    int nearWant = 0;
    int stamped = 0;
    float userX = 0.f;
    float userY = 0.f;
    uint32_t petSendHits = 0;
    uint32_t poolSendHits = 0;
    uint32_t petSendDelta = 0;
    uint32_t poolSendDelta = 0;
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

// 主线程：盖黑名单 LastTryPickUp → DropPool.TryPickUpDrop(userPos)
bool TryFootPickup(float halfW, float halfH, const SkipIds* skipIds, FootResult& out);

}  // namespace x::features::ports::drop
