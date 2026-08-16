// map_attack_port — P2：扩 FindHit 盒到本图 AABB；不抬 maxCount。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "map_attack_port.h"

#include "hit_pin_port.h"
#include "map_bounds_port.h"
#include "mob_pool_port.h"
#include "player_combat_port.h"
#include "world_port.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/log.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace x::features::ports::map_attack {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

constexpr size_t kFbMobId = 0x134;
constexpr int kOidCap = 8;

std::atomic<bool> gOn{false};

struct CallTap {
    bool mutated = false;
    const char* why = "";
    float ox = 0.f, oy = 0.f, ow = 0.f, oh = 0.f;
};

thread_local CallTap tTap{};

int32_t ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void ReadRect(void* rect, float* x, float* y, float* w, float* h) {
    *x = *y = *w = *h = 0.f;
    if (!rect) return;
    __try {
        const auto* p = reinterpret_cast<const float*>(rect);
        *x = p[0];
        *y = p[1];
        *w = p[2];
        *h = p[3];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteRect(void* rect, float x, float y, float w, float h) {
    if (!rect) return;
    __try {
        auto* p = reinterpret_cast<float*>(rect);
        p[0] = x;
        p[1] = y;
        p[2] = w;
        p[3] = h;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* ListFromRef(void* mobsRef) {
    if (!mobsRef) return nullptr;
    void* p = ReadPtr(mobsRef, 0);
    if (LooksLikeHeapPtr(p)) return p;
    return nullptr;
}

int CollectOids(void* mobsRef, int32_t* out, int cap) {
    if (!out || cap <= 0) return 0;
    memset(out, 0, static_cast<size_t>(cap) * sizeof(int32_t));
    void* list = ListFromRef(mobsRef);
    if (!LooksLikeHeapPtr(list)) return 0;
    x::runtime::il2cpp_container::Ensure();
    const int n = ReadI32(list, x::runtime::il2cpp_container::OffListSize());
    if (n <= 0 || n > 64) return 0;
    void* items = ReadPtr(list, x::runtime::il2cpp_container::OffListItems());
    if (!LooksLikeHeapPtr(items)) return 0;
    const uintptr_t alen = x::runtime::il2cpp::ArrayLen(items);
    const int lim = n < static_cast<int>(alen) ? n : static_cast<int>(alen);
    int wrote = 0;
    for (int i = 0; i < lim && wrote < cap; ++i) {
        void* mob = x::runtime::il2cpp::ArrayAt(items, static_cast<uintptr_t>(i));
        if (!LooksLikeHeapPtr(mob)) continue;
        out[wrote++] = ReadI32(mob, kFbMobId);
    }
    return wrote;
}

void OidDelta(int32_t oid, float* dx, float* dy) {
    *dx = 0.f;
    *dy = 0.f;
    if (oid <= 0) return;
    player_combat::CombatCtx ctx{};
    if (!player_combat::QueryCombatCtx(ctx) || !ctx.ok) return;
    mob::Snapshot snap{};
    if (!mob::GetCached(snap) || !snap.ok) return;
    for (int i = 0; i < snap.count; ++i) {
        if (snap.mobs[i].id != oid) continue;
        *dx = snap.mobs[i].x - ctx.x;
        *dy = snap.mobs[i].y - ctx.y;
        return;
    }
}

void OnBefore(void* rect, int32_t* maxCount, int32_t startIndex) {
    (void)maxCount;
    (void)startIndex;
    tTap = CallTap{};
    if (!gOn.load(std::memory_order_acquire) || !rect) return;
    ReadRect(rect, &tTap.ox, &tTap.oy, &tTap.ow, &tTap.oh);

    map_bounds::Rect b{};
    const int mapId = world::GetMapId();
    if (!map_bounds::QueryPlayBounds(mapId, &b) || !b.ok) {
        tTap.why = "no_aabb";
        return;
    }
    const float x = static_cast<float>(b.left);
    const float y = static_cast<float>(b.top);
    const float w = static_cast<float>(b.right - b.left);
    const float h = static_cast<float>(b.bottom - b.top);
    if (!(w > 1.f) || !(h > 1.f) || !std::isfinite(w) || !std::isfinite(h)) {
        tTap.why = "bad_aabb";
        return;
    }
    WriteRect(rect, x, y, w, h);
    float nx = 0.f, ny = 0.f, nw = 0.f, nh = 0.f;
    ReadRect(rect, &nx, &ny, &nw, &nh);
    if (std::fabs(nx - x) > 0.5f || std::fabs(nw - w) > 0.5f) {
        tTap.why = "write_fail";
        return;
    }
    tTap.mutated = true;
    tTap.why = "aabb";
}

void OnAfter(void* rect, void* mobsRef, int32_t n, int32_t maxCount, int32_t startIndex) {
    if (!gOn.load(std::memory_order_acquire)) return;
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    ReadRect(rect, &x, &y, &w, &h);
    int32_t oids[kOidCap]{};
    const int no = CollectOids(mobsRef, oids, kOidCap);
    float dx = 0.f, dy = 0.f;
    OidDelta(oids[0], &dx, &dy);
    x::runtime::LogI("MapAtk",
                     "exp=%d why=%s mc=%d si=%d n=%d no=%d "
                     "xywh=%.1f,%.1f,%.1f,%.1f orig=%.1f,%.1f,%.1f,%.1f "
                     "oid0=%d dx=%.0f dy=%.0f oid=%d,%d,%d,%d,%d,%d,%d,%d",
                     tTap.mutated ? 1 : 0, tTap.why ? tTap.why : "", (int)maxCount,
                     (int)startIndex, (int)n, no, x, y, w, h, tTap.ox, tTap.oy, tTap.ow, tTap.oh,
                     (int)oids[0], dx, dy, (int)oids[0], (int)oids[1], (int)oids[2], (int)oids[3],
                     (int)oids[4], (int)oids[5], (int)oids[6], (int)oids[7]);
}

}  // namespace

void Init() {
    gOn.store(false, std::memory_order_release);
    x::runtime::LogI("MapAtk", "port init (P2 expand AABB, keep maxCount, default off)");
}

void Shutdown() { SetEnabled(false); }

void SetEnabled(bool on) {
    const bool prev = gOn.exchange(on, std::memory_order_acq_rel);
    if (on) {
        hit_pin::SetBeforeFindHit(&OnBefore);
        hit_pin::SetAfterFindHit(&OnAfter);
        hit_pin::SetAuxWanted(true);
    } else {
        hit_pin::SetBeforeFindHit(nullptr);
        hit_pin::SetAfterFindHit(nullptr);
        hit_pin::SetAuxWanted(false);
    }
    if (prev != on) {
        x::runtime::LogI("MapAtk", "enabled=%d expand=1 hit_pin_armed=%d", on ? 1 : 0,
                         hit_pin::IsArmed() ? 1 : 0);
    }
}

bool IsEnabled() { return gOn.load(std::memory_order_acquire); }

}  // namespace x::features::ports::map_attack
