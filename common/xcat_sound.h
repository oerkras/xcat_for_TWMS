#pragma once
// xcat_sound.h — 自研 UI/事件音效（程序化 PCM + waveOut，不依赖 Windows 系统 wav）

#include <cstdint>
#include <vector>

namespace xcat::sound {

enum class Id : uint8_t {
    UiClick = 0,
    UiConfirm,
    UiToggle,
    UiError,
    UiShutdown,
    BuildOk,
    BuildFail,
    LaunchOk,
    LaunchFail,
    FeatureReady,
    Notify,
    Alarm,
    AlarmTimeout,     // 测谎识别/提交超时：与进行中 Alarm 明显区分
    RestrictionAlarm, // 限制 Debuff 首次检出：短促双拍，区别于测谎四拍 Alarm
    LiePass,          // 测谎通过：明亮愉悦上行
    GameContextOk,
    ScrollDrop,       // 卷軸掉落：连续三声叮咚
    Count
};

struct Settings {
    float volume = 0.82f;  // 0..1
    bool  enabled = true;
};

void Init();
void Shutdown();

void SetSettings(const Settings& s);
Settings GetSettings();

const char* IdName(Id id);

// 异步播放（UI / 后台线程安全）
bool Play(Id id);
bool PlayAsync(Id id);

// 清空队列并打断当前播放，立刻播指定音效（测谎通过等抢占场景）
bool PlayInterrupt(Id id);

// 仅打断/清空，不入队（停警报声）
void CancelPlayback();

// 阻塞至播完（build.bat / CLI）
bool PlayBlocking(Id id);

// 播放纯静音（杀进程前排空 DAC / 混音器，减轻 TerminateProcess 爆音）
bool PlaySilenceBlocking(int durationMs);

// CLI：名称别名 success/fail/click/launch-ok/...
bool PlayNamed(const char* name);
bool PlayNamedBlocking(const char* name);

// 自定义 PCM（44100 Hz / 16-bit / mono），走同一条异步队列。
bool PlayPcmAsync(std::vector<int16_t> pcm);

// 清空队列并打断当前播放，立刻播这段 PCM（卷轴口播只留最新一条）。
bool PlayPcmInterrupt(std::vector<int16_t> pcm);

}  // namespace xcat::sound
