#include "map_bounds_port.h"

#include "foothold_port.h"
#include "world_port.h"
#include "../../runtime/log.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace x::features::ports::map_bounds {
namespace {

std::mutex gLogMu;
int gLoggedMapId = 0;

std::mutex gFhMu;
int gFhAabbMapId = 0;
Rect gFhAabb{};
// Snapshot ~100KB：进程内一份，禁止每次 new/delete。
foothold::Snapshot* gFhSnap = nullptr;

bool FhAabbForMap(int mapId, Rect* out) {
    if (!out || mapId <= 0) return false;
    *out = {};
    {
        std::lock_guard<std::mutex> lock(gFhMu);
        if (gFhAabbMapId == mapId && gFhAabb.ok) {
            *out = gFhAabb;
            return true;
        }
    }
    if (!foothold::IsCacheReadyForMap(mapId)) return false;

    std::lock_guard<std::mutex> lock(gFhMu);
    if (!gFhSnap) gFhSnap = new foothold::Snapshot{};
    if (!foothold::GetCached(*gFhSnap) || !gFhSnap->ok || gFhSnap->mapId != mapId ||
        gFhSnap->footholdN <= 0) {
        gFhAabbMapId = 0;
        gFhAabb = {};
        return false;
    }

    int loX = 0, hiX = 0, loY = 0, hiY = 0;
    bool any = false;
    for (int i = 0; i < gFhSnap->footholdN; ++i) {
        const auto& fh = gFhSnap->footholds[i];
        if (fh.id == 0) continue;
        const int a = (std::min)(fh.x1, fh.x2);
        const int b = (std::max)(fh.x1, fh.x2);
        const int c = (std::min)(fh.y1, fh.y2);
        const int d = (std::max)(fh.y1, fh.y2);
        if (!any) {
            loX = a;
            hiX = b;
            loY = c;
            hiY = d;
            any = true;
        } else {
            if (a < loX) loX = a;
            if (b > hiX) hiX = b;
            if (c < loY) loY = c;
            if (d > hiY) hiY = d;
        }
    }
    if (!any || !(loX < hiX && loY < hiY)) {
        gFhAabbMapId = 0;
        gFhAabb = {};
        return false;
    }

    gFhAabb.ok = true;
    gFhAabb.src = "fh";
    gFhAabb.left = loX;
    gFhAabb.top = loY;
    gFhAabb.right = hiX;
    gFhAabb.bottom = hiY;
    gFhAabbMapId = mapId;
    *out = gFhAabb;
    return true;
}

void MaybeLogBounds(int mapId, const Rect& r) {
    if (!r.ok || mapId <= 0) return;
    std::lock_guard<std::mutex> lock(gLogMu);
    if (gLoggedMapId == mapId) return;
    gLoggedMapId = mapId;
    x::runtime::LogI("MapBounds", "play bounds map=%d src=%s L=%d T=%d R=%d B=%d", mapId,
                     r.src ? r.src : "?", r.left, r.top, r.right, r.bottom);
}

}  // namespace

void InvalidateFhAabbCache() {
    {
        std::lock_guard<std::mutex> lock(gFhMu);
        gFhAabbMapId = 0;
        gFhAabb = {};
    }
    {
        std::lock_guard<std::mutex> lock(gLogMu);
        gLoggedMapId = 0;
    }
}

bool QueryPlayBounds(int mapId, Rect* out) {
    if (!out) return false;
    *out = {};
    if (mapId <= 0) mapId = world::GetMapId();
    if (mapId <= 0) return false;

    // 产品闸只信本图 FH AABB。离线 VR（map_info.tsv）已证不可靠：
    // BIN 107000402 src=vr B=250 把刷怪地板 Y≈300+ 全杀成 noLand；
    // 真·飞出图软重载由 fill_slim 收态解决，不靠 VR。
    // FH 未 Collect 时返回 false → PointInPlayBounds 放行（不误杀；Snap 仍是主闸）。
    if (FhAabbForMap(mapId, out)) {
        MaybeLogBounds(mapId, *out);
        return true;
    }
    return false;
}

bool PointInPlayBounds(float x, float y, int mapId, int marginPx) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    Rect r{};
    if (!QueryPlayBounds(mapId, &r) || !r.ok) {
        // 无边界数据（含 FH 尚未 Collect）：不拦。
        return true;
    }
    const int m = marginPx > 0 ? marginPx : 0;
    const float L = static_cast<float>(r.left + m);
    const float R = static_cast<float>(r.right - m);
    const float T = static_cast<float>(r.top + m);
    const float B = static_cast<float>(r.bottom - m);
    if (!(L < R && T < B)) return true;
    return x >= L && x <= R && y >= T && y <= B;
}

}  // namespace x::features::ports::map_bounds
