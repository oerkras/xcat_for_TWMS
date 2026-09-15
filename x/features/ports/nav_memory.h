#pragma once
// 经典版 / TWMS — 拟人寻路 / 寻怪的**跨会话记忆**（bin\state\navmem\）。
// 进程里学到的东西以前换图即清、重启全忘：同一张图每天前 5 分钟都在重犯同样的错——
// 高绳下面跳 3 次才知道够不着、守点要先量几轮才知道刷怪间隔、耗时模型永远是拍脑袋的常数。
//
//   map_<mapId>.txt   死边（跳 3 次够不着的绳 / 摔回 3 次的极限跳）+ 刷怪间隔 EMA
//   eta_scale.txt     耗时模型按边类型的实测校准比例（全局，不分图）
//
// 纯文本 key=value，一行一项；写盘先写 .tmp 再替换。所有接口线程安全、失败静默（记忆丢了
// 不影响功能，只是回到「现学」）。

#include <cstdint>

namespace x::features::ports::nav_memory {

constexpr int kDeadEdgeCap = 32;
constexpr int kEtaKinds = 8;  // EdgeKind 0..5 用到，留余量

struct DeadEdgeRec {
    uint32_t from = 0;
    uint32_t to = 0;
    uint8_t kind = 0;
};

struct MapMemory {
    int32_t mapId = 0;
    float respawnEmaMs = 0.f;
    int respawnSamples = 0;
    int deadN = 0;
    DeadEdgeRec dead[kDeadEdgeCap]{};
};

// 该图的记忆（有文件读文件，没有就是空的）。
bool Load(int32_t mapId, MapMemory* out);

// 刷怪间隔学到新值：合并进该图记忆，30s 内最多落盘一次（换图 / Flush 时立即）。
void NoteRespawn(int32_t mapId, float emaMs, int samples);

// 学到一条物理上过不去的边：立即落盘（很少发生，且最值钱）。
void NoteDeadEdge(int32_t mapId, uint32_t from, uint32_t to, uint8_t kind);

// 耗时校准比例（按 EdgeKind）：读 / 写。n ≤ kEtaKinds。
bool LoadEtaScale(float* scale, int* samples, int n);
void SaveEtaScale(const float* scale, const int* samples, int n);

// 跳键延迟 EMA（ms，全局）：读 / 写。每台机器基本是常数，重启后第一跳就该带着提前量，
// 不然每次开程序头几跳都因没校准而起跳晚 8~12px（100ms 延迟下极限跳落到台下）。
bool LoadJumpLatency(float* ms, int* samples);
void SaveJumpLatency(float ms, int samples);

// 把还没落盘的改动写掉（卸载前）。
void Flush();

}  // namespace x::features::ports::nav_memory
