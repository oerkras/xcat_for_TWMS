#include "reach_cal.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace x::features::simple_combat::reach {
namespace {

// 分档上沿。与离线脚本 `_reach.py` 的 BINS 一致，便于日志直接并表核对。
constexpr float kEdges[] = {20.f, 35.f, 50.f, 65.f, 80.f, 100.f, 130.f};
constexpr int kNBin = static_cast<int>(sizeof(kEdges) / sizeof(kEdges[0]));

// 一档要多少（衰减后的）样本才算数。太小会被噪声带跑，太大则跟不上换图。
constexpr float kMinBinN = 60.f;
// 跌破台面多少个点算断崖。离线用 15 点，这里同口径。
constexpr float kDropPts = 15.f;
// 至少多少总样本才敢偏离 fallback。
constexpr float kMinTotal = 300.f;

// ★★ 当前为**只观测、不接管**。StationDx 恒返回 fallback，估计值只进日志。
//
// 为什么：把估计器拿 44 台客户机的历史日志做了逐窗回放（`Dumps/runtime/_calsim.py`），
// 三条硬伤，不敢让它驱动站位——
//   ① **该救的没救到**：231D8D4DD1044AA_ca9a5e61（离线断崖@20、台面仅 80.6%）全程维持 40；
//      B02_c2a311b8（离线断崖@80）同样一次没动。远档凑不满 kMinBinN，断崖判定压根没触发。
//   ② **大样本上剧烈抖动**：008d6f7a 变动 171 次、e2e62cca 变动 213 次，门在 30↔48↔60 之间
//      来回跳。站位目标飘忽会把 MoveTo↔Firing 往返顶上去（BIN 1394b0 的老坑）。
//   ③ **方向会反**：e2e62cca 离线是断崖@65，在线却收敛到 60（比兜底 40 还松）——远档样本被
//      衰减吃掉后断崖判定时断时续，一断就落进「没断崖只许放宽」分支。
// 且两个方向都不安全：收紧加剧往返；放宽会把站位顶到真实包线边缘，跟踪误差一口吃掉余量。
//
// 解除接管的前置条件（缺一不可）：
//   · 输出加迟滞/限摆，回放里单台变动次数降到个位数；
//   · 离线判定有断崖的那 7 台，在线也能稳定测到，且方向一致；
//   · 至少 3 台真机跑过一轮，日志里的 reach_cal 行与 `_reach.py` 的逐档表能对上。
constexpr bool kActuate = false;

// 指数衰减：每 kDecayEvery 次判决把全部计数乘以 kDecayFactor。
// 有效窗口 ≈ kDecayEvery/(1-kDecayFactor) ≈ 8000 个窗。原为 400/0.75（≈1600）——太短，
// 远档凑不满样本，正是上面①③两条的根因；观测期先放长，让日志里的数可用。
constexpr int kDecayEvery = 1200;
constexpr float kDecayFactor = 0.85f;

// 站位取包线的六成：留出旋翼跟踪误差与怪自身走动的余量。
// 该系数下我方主测机（edge=65）解出 39 ≈ 现行常数 40，即**对现状是恒等变换**，
// 只有射程明显更长/更短的角色才会被挪动。改它之前先想清楚这条锚。
constexpr float kStationFrac = 0.60f;
constexpr float kStationMin = 30.f;  // 再紧会让 MoveTo↔Firing 往返变多（BIN 1394b0）
constexpr float kStationMax = 90.f;

struct Bin {
    float n = 0.f;
    float hit = 0.f;
};

std::mutex gMu;
Bin gBin[kNBin];
int gSinceDecay = 0;
float gStation = 0.f;   // 0 = 尚未解出
float gPlateau = 0.f;
float gEdge = 0.f;
bool gCliff = false;

int BinOf(float absDx) {
    for (int i = 0; i < kNBin; ++i) {
        if (absDx < kEdges[i]) return i;
    }
    return -1;  // 超出最远档：不参与统计（出刀闸 120 之外本就不该有刀）
}

float TotalLocked() {
    float t = 0.f;
    for (int i = 0; i < kNBin; ++i) t += gBin[i].n;
    return t;
}

// 解算：台面 = 首个够样本的档；从近到远走，首个跌破台面 kDropPts 的档即断崖。
void SolveLocked() {
    gPlateau = 0.f;
    gEdge = 0.f;
    gCliff = false;

    int base = -1;
    for (int i = 0; i < kNBin; ++i) {
        if (gBin[i].n >= kMinBinN) {
            base = i;
            break;
        }
    }
    if (base < 0) return;
    gPlateau = 100.f * gBin[base].hit / gBin[base].n;

    float reachHi = kEdges[base];
    for (int i = base; i < kNBin; ++i) {
        if (gBin[i].n < kMinBinN) continue;
        const float r = 100.f * gBin[i].hit / gBin[i].n;
        if (r < gPlateau - kDropPts) {
            // 断崖档的**下沿**才是还打得到的边界。
            gEdge = (i == 0) ? kEdges[0] : kEdges[i - 1];
            gCliff = true;
            return;
        }
        reachHi = kEdges[i];
    }
    gEdge = reachHi;  // 观察范围内没掉：边界至少有这么远
}

}  // namespace

void Feed(float absDx, bool hit) {
    if (!std::isfinite(absDx) || absDx < 0.f) return;
    const int b = BinOf(absDx);
    if (b < 0) return;

    std::lock_guard<std::mutex> lk(gMu);
    gBin[b].n += 1.f;
    if (hit) gBin[b].hit += 1.f;

    if (++gSinceDecay >= kDecayEvery) {
        gSinceDecay = 0;
        for (int i = 0; i < kNBin; ++i) {
            gBin[i].n *= kDecayFactor;
            gBin[i].hit *= kDecayFactor;
        }
    }
    SolveLocked();

    if (TotalLocked() < kMinTotal || gEdge <= 0.f) {
        gStation = 0.f;
        return;
    }
    gStation = std::clamp(gEdge * kStationFrac, kStationMin, kStationMax);
}

float StationDx(float fallback) {
    if (!kActuate) return fallback;  // 观测期：估计值只进日志，不驱动站位（理由见 kActuate）
    std::lock_guard<std::mutex> lk(gMu);
    if (gStation <= 0.f) return fallback;
    // ★ 安全不对称：没测到断崖就只许放宽，不许收紧。
    // 「这局没飞到那么远」与「飞过去打不到」在数据上长得一样，把前者误判成后者会让站位
    // 越缩越死，还会把 MoveTo↔Firing 往返推上去。
    if (!gCliff) return std::max(gStation, fallback);
    return gStation;
}

Snap Read() {
    std::lock_guard<std::mutex> lk(gMu);
    Snap s{};
    s.samples = static_cast<int>(TotalLocked() + 0.5f);
    s.plateau = gPlateau;
    s.edge = gEdge;
    s.cliff = gCliff;
    s.stationDx = gStation;
    s.ready = gStation > 0.f;
    return s;
}

void Reset() {
    std::lock_guard<std::mutex> lk(gMu);
    for (int i = 0; i < kNBin; ++i) gBin[i] = Bin{};
    gSinceDecay = 0;
    gStation = 0.f;
    gPlateau = 0.f;
    gEdge = 0.f;
    gCliff = false;
}

}  // namespace x::features::simple_combat::reach
