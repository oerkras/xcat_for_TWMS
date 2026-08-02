#pragma once
// pet_port — Classic TWMS 宠物只读态 + 自动召唤（P0c）
//
// 范围：自动召唤；喂食交给官方（阈值 50），TryFeed 不做。
// 真源：docs/features/pet_feed/P0a_锚点复核.md
// 禁止 INLINE HOOK。

namespace x::features::ports::pet {

struct PetCareState {
    bool hasLocalUser = false;
    bool mapDisablesPet = false;
    int activatedCount = 0;
    int minRepleteness = -1;
    bool hasFood = false;
    int foodItemId = 0;
    int foodPos = 0;
    int foodQty = 0;
    int summonPetPos = 0;
    int deadPetCount = 0;
    int cashPetCount = 0;
    // 首只 Cash 宠诊断（永久宠 remainLife 常为 0，勿当死亡）
    int probeRemainLife = -1;
    int probeActiveState = -1;
    int probeDeadByDate = -1;
};

void Init();
void Shutdown();
bool EnsureBound();
bool ReadState(PetCareState& out);

// 主线程泵 → CashItemManager.SendActivatePetRequest(nPos)
bool TryActivatePet(int nPos);

bool TryFeed();  // 首版恒 false

}  // namespace x::features::ports::pet
