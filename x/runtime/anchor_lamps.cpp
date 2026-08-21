#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "anchor_lamps.h"

#include "il2cpp_shape.h"
#include "main_thread_pump.h"

#include <Windows.h>

#include <cstring>
#include <mutex>

namespace x::runtime::anchor_lamps {
namespace {

constexpr size_t kSlots = xcat::kAnchorLampMax;

// 固定展示顺序：shape → 泵 → feature。Publish 时一律预挂灰灯，避免「未 Set 就不出现」。
const char* const kOrder[] = {"WM",         "UL",         "NM",         "FAC",
                              "SA",         "Pump",       "FlyCam",     "TravelSend",
                              "ShopUnity",  "AttackFK",   "ChanHop",    "DropAlert",
                              "InputOnKey", "WorldMap",   "Consumable", "Teleport",
                              "PetAct",     "PetRect",    "DropFields", "SkillMax",
                              "FaForce",    "InfStars",   "ForceTrade"};

struct Slot {
    char id[xcat::kAnchorLampIdLen]{};
    char detail[xcat::kAnchorLampDetailLen]{};
    AnchorLampCode code = AnchorLampCode::Unknown;
    bool used = false;
};

std::mutex gMu;
Slot gSlots[kSlots]{};

void CopyTrunc(char* dst, size_t n, const char* src) {
    if (!dst || n == 0) return;
    dst[0] = '\0';
    if (!src) return;
    strncpy_s(dst, n, src, _TRUNCATE);
}

Slot* FindOrAlloc(const char* id) {
    if (!id || !id[0]) return nullptr;
    for (size_t i = 0; i < kSlots; ++i) {
        if (gSlots[i].used && _stricmp(gSlots[i].id, id) == 0) return &gSlots[i];
    }
    for (size_t i = 0; i < kSlots; ++i) {
        if (!gSlots[i].used) {
            gSlots[i].used = true;
            CopyTrunc(gSlots[i].id, sizeof(gSlots[i].id), id);
            gSlots[i].code = AnchorLampCode::Unknown;
            gSlots[i].detail[0] = '\0';
            return &gSlots[i];
        }
    }
    return nullptr;
}

void SetLocked(const char* id, AnchorLampCode code, const char* detail) {
    Slot* s = FindOrAlloc(id);
    if (!s) return;
    s->code = code;
    CopyTrunc(s->detail, sizeof(s->detail), detail ? detail : "");
}

void EnsureRosterLocked() {
    for (const char* id : kOrder) {
        Slot* s = FindOrAlloc(id);
        // 仅给新槽写 pending；已 Set 的绿/黄/红/明细不动。
        if (s && s->code == AnchorLampCode::Unknown && !s->detail[0]) {
            CopyTrunc(s->detail, sizeof(s->detail), "pending");
        }
    }
}

AnchorLampCode PathToCode(x::runtime::il2cpp_shape::ResolvePath p) {
    using RP = x::runtime::il2cpp_shape::ResolvePath;
    switch (p) {
        case RP::Hash:
            return AnchorLampCode::Ok;
        case RP::Shape:
            return AnchorLampCode::Degraded;
        default:
            return AnchorLampCode::Miss;
    }
}

void SampleCoreLocked() {
    x::runtime::il2cpp_shape::LogResolveSelfCheck();
    const auto paths = x::runtime::il2cpp_shape::GetCachedPaths();
    SetLocked("WM", PathToCode(paths.wm), x::runtime::il2cpp_shape::PathName(paths.wm));
    SetLocked("UL", PathToCode(paths.ul), x::runtime::il2cpp_shape::PathName(paths.ul));
    SetLocked("NM", PathToCode(paths.nm), x::runtime::il2cpp_shape::PathName(paths.nm));
    SetLocked("FAC", PathToCode(paths.fac), x::runtime::il2cpp_shape::PathName(paths.fac));
    SetLocked("SA", PathToCode(paths.sa), x::runtime::il2cpp_shape::PathName(paths.sa));

    const bool pump = x::runtime::main_thread::IsInstalled();
    SetLocked("Pump", pump ? AnchorLampCode::Ok : AnchorLampCode::Miss, pump ? "installed" : "MISS");
}

}  // namespace

void Set(const char* id, AnchorLampCode code, const char* detail) {
    std::lock_guard<std::mutex> lock(gMu);
    SetLocked(id, code, detail);
}

void Publish(const char* binDir) {
    if (!binDir || !binDir[0]) return;

    xcat::AnchorLampsStatus st{};
    xcat::AnchorLampsSetDefaults(st);
    {
        std::lock_guard<std::mutex> lock(gMu);
        // 先占位整表灰灯，再采样/合并 feature Set —— 未绑定也能在 ImGui 看见名字。
        EnsureRosterLocked();
        SampleCoreLocked();
        for (const char* id : kOrder) {
            for (size_t i = 0; i < kSlots && st.count < xcat::kAnchorLampMax; ++i) {
                if (!gSlots[i].used || _stricmp(gSlots[i].id, id) != 0) continue;
                auto& e = st.entries[st.count++];
                CopyTrunc(e.id, sizeof(e.id), gSlots[i].id);
                CopyTrunc(e.detail, sizeof(e.detail), gSlots[i].detail);
                e.code = static_cast<uint8_t>(gSlots[i].code);
                break;
            }
        }
        for (size_t i = 0; i < kSlots && st.count < xcat::kAnchorLampMax; ++i) {
            if (!gSlots[i].used) continue;
            bool already = false;
            for (uint32_t j = 0; j < st.count; ++j) {
                if (_stricmp(st.entries[j].id, gSlots[i].id) == 0) {
                    already = true;
                    break;
                }
            }
            if (already) continue;
            auto& e = st.entries[st.count++];
            CopyTrunc(e.id, sizeof(e.id), gSlots[i].id);
            CopyTrunc(e.detail, sizeof(e.detail), gSlots[i].detail);
            e.code = static_cast<uint8_t>(gSlots[i].code);
        }
    }
    st.writeTickMs = GetTickCount64();
    (void)xcat::WriteAnchorLamps(binDir, st);
}

}  // namespace x::runtime::anchor_lamps
