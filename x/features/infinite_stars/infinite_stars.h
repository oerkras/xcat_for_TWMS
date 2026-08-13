#pragma once
// 经典版 TWMS —— 实验·无限飞镖
//
// 用户入口已关（kInfiniteStarsUserEnabled=false）：不启 worker、不挂钩。
// 代码保留；重开时把该常量改回 true。
//
// 主路径：自动维持夜使者 4 转 4121006（Spirit Javelin / 无形镖，台服常称無限飛鏢）。
// 服端认这个 BUFF 时不扣飞镖。未学会则只冻客户端 207xxxx 数量（服端仍扣）。

namespace x {
namespace features {
namespace infinite_stars {

void Init();
void Shutdown();
void StartWorker();
void StopWorker();

void SetEnabled(bool on);
bool IsEnabled();

}  // namespace infinite_stars
}  // namespace features
}  // namespace x
