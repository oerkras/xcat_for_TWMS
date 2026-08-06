#include "mono_clock.h"

namespace x::runtime {
namespace {

LONGLONG gQpcFreq = 0;    // 0 = QPC 不可用，退化 GetTickCount
LONGLONG gQpcAnchor = 0;
DWORD gTickAnchor = 0;
INIT_ONCE gAnchorOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK AnchorOnce(PINIT_ONCE, PVOID, PVOID*) {
    LARGE_INTEGER freq{};
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0) return TRUE;

    // 自旋到 GetTickCount 的下一次步进：在边界处采样 QPC，锚点残差 <1ms。
    // 直接取当前值会带来最多一整个 tick（~15.6ms）的固定偏移，那会让混用旧时间戳的
    // 门控（如 50ms）实际变成 34ms 或 66ms —— 比原来的量化问题更糟。
    const DWORD t0 = GetTickCount();
    LARGE_INTEGER q{};
    DWORD t1 = t0;
    do {
        YieldProcessor();
        t1 = GetTickCount();
        QueryPerformanceCounter(&q);
    } while (t1 == t0);

    gQpcFreq = freq.QuadPart;
    gQpcAnchor = q.QuadPart;
    gTickAnchor = t1;
    return TRUE;
}

void EnsureAnchored() {
    InitOnceExecuteOnce(&gAnchorOnce, AnchorOnce, nullptr, nullptr);
}

}  // namespace

DWORD NowMs() {
    EnsureAnchored();
    if (gQpcFreq <= 0) return GetTickCount();

    LARGE_INTEGER q{};
    if (!QueryPerformanceCounter(&q)) return GetTickCount();

    // 以锚点为基做差再换算，避免 QPC 绝对值 ×1000 溢出（10MHz 下 d 需跑 ~29 年才碰 int64 上限）。
    const LONGLONG d = q.QuadPart - gQpcAnchor;
    return gTickAnchor + static_cast<DWORD>((d * 1000) / gQpcFreq);
}

void WarmUpClock() { EnsureAnchored(); }

}  // namespace x::runtime
