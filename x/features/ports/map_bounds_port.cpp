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

// 该地板在 x 列上的高度。斜坡必须插值——拿端点近似会在长斜坡上错出上百 px。
// 返回 false 表示这块地板不覆盖 x，或它是竖直墙段（x1==x2，不是地板）。
bool FloorYAt(const foothold::FootholdLite& fh, float x, float* yOut) {
    if (fh.id == 0 || fh.x1 == fh.x2) return false;
    const float ax = static_cast<float>((std::min)(fh.x1, fh.x2));
    const float bx = static_cast<float>((std::max)(fh.x1, fh.x2));
    if (x < ax || x > bx) return false;
    const float t = (x - static_cast<float>(fh.x1)) /
                    static_cast<float>(fh.x2 - fh.x1);
    *yOut = static_cast<float>(fh.y1) +
            t * static_cast<float>(fh.y2 - fh.y1);
    return true;
}

// 在 x 列上找 y 之下最近的地板。found=false ⇒ 掉落区。
// 调用方必须已持有 gFhMu，且已确认 gFhSnap 对齐当前图。
// ★ 坐标是 **+Y 向上**（实测，非注释推断）：日志里 vy>0 的样本其后 y 增大占 84.8%、
//   vy<0 的占 18.8%（y 与 vy 同向），而下坠恒为负 vy（BIN 2d6176 用户实机印证）。
//   ⇒ 脚下的地板是 **fy ≤ y**，最近的一块是其中**最大**的 fy。
//
//   本函数原先写成 `fy < y ⇒ continue` + 取最小，那是 +Y 向下的写法，与本工程实际
//   相反。它当时唯一的调用方 MaybeLogBounds 传 `r.top-1`（全图之下），在反语义下
//   恰好答对了「这列有没有地板」，所以掉落区检测一直是对的、错的只是契约——
//   一旦有第二个调用方按字面语义用它就会翻车。现已摆正，调用方同步改为从图顶往下问。
bool FloorBelowLocked(float x, float y, float* floorYOut) {
    bool found = false;
    float best = 0.f;
    for (int i = 0; i < gFhSnap->footholdN; ++i) {
        float fy = 0.f;
        if (!FloorYAt(gFhSnap->footholds[i], x, &fy)) continue;
        if (fy > y) continue;  // +Y 向上：地板要在脚下 ⇒ 不高于当前高度
        if (!found || fy > best) {
            best = fy;
            found = true;
        }
    }
    if (found && floorYOut) *floorYOut = best;
    return found;
}

void MaybeLogBounds(int mapId, const Rect& r) {
    if (!r.ok || mapId <= 0) return;
    std::lock_guard<std::mutex> lock(gLogMu);
    if (gLoggedMapId == mapId) return;
    gLoggedMapId = mapId;
    x::runtime::LogI("MapBounds", "play bounds map=%d src=%s L=%d T=%d R=%d B=%d", mapId,
                     r.src ? r.src : "?", r.left, r.top, r.right, r.bottom);

    // 顺带把「矩形内部的空洞」摊开：以 32px 扫一遍列，统计完全没有地板的列。
    // 这些列就是掉落区——AABB 判它们「界内」，实际踩进去会掉出图。
    // 每图只打一行，用来在真实日志里核对本判据是否认得出事故点。
    std::lock_guard<std::mutex> fhLock(gFhMu);
    if (!gFhSnap || gFhSnap->mapId != mapId || gFhSnap->footholdN <= 0) return;
    const float lo = static_cast<float>(r.left);
    const float hi = static_cast<float>(r.right);
    const float step = 32.f;
    int cols = 0, voids = 0;
    float firstVoid = 0.f, lastVoid = 0.f;
    for (float x = lo; x <= hi; x += step) {
        ++cols;
        // 从矩形顶端往下看：整列都没有地板 ⇒ 空洞列。
        // +Y 向上 ⇒ 图顶是 bottom（数值最大），从它上方 1px 起往下问。
        if (FloorBelowLocked(x, static_cast<float>(r.bottom) + 1.f, nullptr)) continue;
        if (!voids) firstVoid = x;
        lastVoid = x;
        ++voids;
    }
    x::runtime::LogI("MapBounds",
                     "danger map=%d cols=%d void=%d (%.0f%%) span=[%.0f,%.0f] step=%.0f", mapId,
                     cols, voids, cols ? 100.0 * voids / cols : 0.0, firstVoid, lastVoid, step);
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

bool HasFloorBelow(float x, float y, int mapId, float* floorYOut) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (mapId <= 0) mapId = world::GetMapId();
    Rect r{};
    // 借 FhAabbForMap 把 gFhSnap 拉起来并对齐 mapId；拿不到数据就不拦。
    if (!FhAabbForMap(mapId, &r) || !r.ok) return true;
    std::lock_guard<std::mutex> lock(gFhMu);
    if (!gFhSnap || gFhSnap->mapId != mapId || gFhSnap->footholdN <= 0) return true;
    return FloorBelowLocked(x, y, floorYOut);
}

bool NearestFloorColumn(float x, float y, int mapId, float maxScanPx, float* safeXOut) {
    if (!safeXOut || !std::isfinite(x) || !std::isfinite(y)) return false;
    if (mapId <= 0) mapId = world::GetMapId();
    Rect r{};
    if (!FhAabbForMap(mapId, &r) || !r.ok) return false;
    std::lock_guard<std::mutex> lock(gFhMu);
    if (!gFhSnap || gFhSnap->mapId != mapId || gFhSnap->footholdN <= 0) return false;

    // 由近及远向两侧扫，先命中先返回——自救要就近，不能横穿半张图。
    const float step = 16.f;
    const float lo = static_cast<float>(r.left);
    const float hi = static_cast<float>(r.right);
    for (float d = step; d <= maxScanPx; d += step) {
        const float l = x - d;
        if (l >= lo && FloorBelowLocked(l, y, nullptr)) {
            *safeXOut = l;
            return true;
        }
        const float rr = x + d;
        if (rr <= hi && FloorBelowLocked(rr, y, nullptr)) {
            *safeXOut = rr;
            return true;
        }
    }
    return false;
}

}  // namespace x::features::ports::map_bounds
