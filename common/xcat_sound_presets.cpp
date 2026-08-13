#include "xcat_sound_presets.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace xcat::sound::presets {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float EnvelopeMs(float tMs, float attackMs, float releaseMs, float durationMs) {
    if (tMs < 0.f || tMs >= durationMs) return 0.f;
    if (attackMs > 0.f && tMs < attackMs) return tMs / attackMs;
    if (releaseMs > 0.f && tMs > durationMs - releaseMs) {
        const float tail = durationMs - tMs;
        return tail > 0.f ? tail / releaseMs : 0.f;
    }
    return 1.f;
}

uint32_t Lcg(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}

float TotalDurationMs(const std::vector<Tone>& tones, const std::vector<NoiseBurst>& noise,
                      float tailPadMs) {
    float end = 0.f;
    for (const auto& t : tones) end = std::max(end, t.startMs + t.durationMs);
    for (const auto& n : noise) end = std::max(end, n.startMs + n.durationMs);
    return end + std::max(0.f, tailPadMs);
}

}  // namespace

std::vector<int16_t> Synthesize(const std::vector<Tone>& tones, const std::vector<NoiseBurst>& noise,
                                float tailPadMs) {
    const float durMs = TotalDurationMs(tones, noise, tailPadMs);
    const int samples = std::max(1, static_cast<int>(durMs * kSampleRate / 1000.f));
    std::vector<float> mix(static_cast<size_t>(samples), 0.f);

    for (const auto& tone : tones) {
        const int i0 = static_cast<int>(tone.startMs * kSampleRate / 1000.f);
        const int n  = static_cast<int>(tone.durationMs * kSampleRate / 1000.f);
        for (int i = 0; i < n; ++i) {
            const int idx = i0 + i;
            if (idx < 0 || idx >= samples) continue;
            const float tSec = static_cast<float>(i) / static_cast<float>(kSampleRate);
            const float tMs  = static_cast<float>(i) * 1000.f / static_cast<float>(kSampleRate);
            const float env =
                EnvelopeMs(tMs, tone.attackMs, tone.releaseMs, tone.durationMs);
            mix[static_cast<size_t>(idx)] +=
                std::sinf(2.f * kPi * tone.freqHz * tSec) * env * tone.gain;
        }
    }

    for (const auto& nb : noise) {
        uint32_t seed = nb.seed;
        const int i0 = static_cast<int>(nb.startMs * kSampleRate / 1000.f);
        const int n  = static_cast<int>(nb.durationMs * kSampleRate / 1000.f);
        for (int i = 0; i < n; ++i) {
            const int idx = i0 + i;
            if (idx < 0 || idx >= samples) continue;
            const float tMs = static_cast<float>(i) * 1000.f / static_cast<float>(kSampleRate);
            const float env = EnvelopeMs(tMs, nb.attackMs, nb.releaseMs, nb.durationMs);
            const float r   = (static_cast<int32_t>(Lcg(seed) & 0xFFFF) - 32768) / 32768.f;
            mix[static_cast<size_t>(idx)] += r * env * nb.gain;
        }
    }

    float peak = 0.f;
    for (float v : mix) peak = std::max(peak, std::fabs(v));
    const float norm = peak > 1.f ? 1.f / peak : 1.f;

    std::vector<int16_t> out(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        const float v = std::clamp(mix[static_cast<size_t>(i)] * norm, -1.f, 1.f);
        out[static_cast<size_t>(i)] = static_cast<int16_t>(v * 32767.f);
    }

    // 尾部线性淡出至 0，避免 DAC 截断产生爆音
    const int fadeN = std::min(samples, static_cast<int>(10.f * kSampleRate / 1000.f));
    for (int i = 0; i < fadeN; ++i) {
        const int idx = samples - 1 - i;
        const float g = static_cast<float>(i) / static_cast<float>(fadeN);
        out[static_cast<size_t>(idx)] =
            static_cast<int16_t>(static_cast<float>(out[static_cast<size_t>(idx)]) * g);
    }
    return out;
}

std::vector<int16_t> Generate(Id id) {
    switch (id) {
    case Id::UiClick:
        // 中低频短音 + 弱泛音，避免 3kHz 玻璃感
        return Synthesize({
            {520.f, 0.f, 38.f, 0.24f, 5.f, 28.f},
            {1040.f, 2.f, 22.f, 0.07f, 3.f, 16.f},
        });
    case Id::UiConfirm:
        return Synthesize({
            {740.f, 0.f, 95.f, 0.26f, 6.f, 40.f},
            {988.f, 75.f, 125.f, 0.22f, 6.f, 55.f},
        });
    case Id::UiToggle:
        return Synthesize({{620.f, 0.f, 48.f, 0.22f, 5.f, 32.f}});
    case Id::UiError:
        return Synthesize({{220.f, 0.f, 180.f, 0.38f, 2.f, 80.f}},
                          std::vector<NoiseBurst>{{0.f, 90.f, 0.18f, 1.f, 40.f, 0xC0FFEE01u}});
    case Id::UiShutdown:
        // 下行双音 + 尾部静音垫（一次 PCM），后台播完再杀进程
        return Synthesize({
            {320.f, 0.f, 65.f, 0.16f, 12.f, 40.f},
            {240.f, 35.f, 85.f, 0.12f, 14.f, 50.f},
        }, {}, 70.f);
    case Id::BuildOk:
        // 尾部静音垫：build.bat 经 xcat_sound.exe 播完即退出，垫长一点减轻关进程爆音
        return Synthesize({
            {523.25f, 0.f, 110.f, 0.30f, 5.f, 40.f},
            {659.25f, 85.f, 110.f, 0.28f, 5.f, 45.f},
            {783.99f, 170.f, 160.f, 0.26f, 6.f, 70.f},
            {1046.5f, 260.f, 200.f, 0.18f, 8.f, 90.f},
        }, {}, 80.f);
    case Id::BuildFail:
        return Synthesize({
            {311.f, 0.f, 140.f, 0.34f, 2.f, 60.f},
            {233.f, 100.f, 180.f, 0.36f, 2.f, 90.f},
        }, std::vector<NoiseBurst>{{0.f, 120.f, 0.14f, 1.f, 50.f, 0xBADF00Du}}, 80.f);
    case Id::LaunchOk:
        return Synthesize({
            {440.f, 0.f, 100.f, 0.28f, 4.f, 35.f},
            {554.37f, 75.f, 100.f, 0.26f, 4.f, 40.f},
            {659.25f, 150.f, 140.f, 0.24f, 5.f, 60.f},
            {880.f, 230.f, 220.f, 0.16f, 8.f, 100.f},
        });
    case Id::LaunchFail:
        return Synthesize({
            {196.f, 0.f, 160.f, 0.36f, 2.f, 70.f},
            {155.f, 120.f, 200.f, 0.34f, 2.f, 100.f},
        }, std::vector<NoiseBurst>{{10.f, 100.f, 0.12f, 1.f, 45.f, 0xDEADBEEFu}});
    case Id::FeatureReady:
        return Synthesize({
            {520.f, 0.f, 55.f, 0.24f, 4.f, 22.f},
            {780.f, 40.f, 70.f, 0.22f, 5.f, 30.f},
            {1040.f, 85.f, 90.f, 0.18f, 6.f, 40.f},
        });
    case Id::Notify:
        return Synthesize({
            {1000.f, 0.f, 70.f, 0.30f, 2.f, 25.f},
            {1500.f, 90.f, 80.f, 0.28f, 2.f, 30.f},
        });
    case Id::Alarm:
        // 人工介入警报：高低频交替四拍，辅以弱二次谐波；明显区别于普通通知双音。
        return Synthesize({
            {620.f, 0.f, 180.f, 0.42f, 5.f, 24.f},
            {1240.f, 0.f, 180.f, 0.10f, 5.f, 24.f},
            {980.f, 190.f, 180.f, 0.42f, 5.f, 24.f},
            {1960.f, 190.f, 180.f, 0.08f, 5.f, 24.f},
            {620.f, 380.f, 180.f, 0.42f, 5.f, 24.f},
            {1240.f, 380.f, 180.f, 0.10f, 5.f, 24.f},
            {980.f, 570.f, 220.f, 0.44f, 5.f, 36.f},
            {1960.f, 570.f, 220.f, 0.08f, 5.f, 36.f},
        }, std::vector<NoiseBurst>{{0.f, 790.f, 0.025f, 2.f, 35.f, 0xA11A4E01u}},
           45.f);
    case Id::AlarmTimeout:
        // 识别超时：三声下行低鸣 + 噪声尾，与进行中高低交替 Alarm 一听可辨。
        return Synthesize({
            {480.f, 0.f, 160.f, 0.48f, 4.f, 50.f},
            {320.f, 170.f, 180.f, 0.50f, 4.f, 60.f},
            {200.f, 360.f, 260.f, 0.52f, 4.f, 90.f},
            {140.f, 540.f, 220.f, 0.40f, 6.f, 110.f},
        }, std::vector<NoiseBurst>{
               {0.f, 200.f, 0.10f, 1.5f, 55.f, 0x71E00101u},
               {350.f, 320.f, 0.14f, 1.2f, 70.f, 0x71E00102u},
           },
           80.f);
    case Id::RestrictionAlarm:
        // 可疑/限制 Debuff：短促双拍（约 0.35s），比测谎四拍 Alarm 更短、音高更尖，一听可辨。
        return Synthesize({
            {880.f, 0.f, 110.f, 0.50f, 3.f, 18.f},
            {1760.f, 0.f, 110.f, 0.14f, 3.f, 18.f},
            {720.f, 130.f, 150.f, 0.52f, 3.f, 28.f},
            {1440.f, 130.f, 150.f, 0.12f, 3.f, 28.f},
        }, std::vector<NoiseBurst>{{0.f, 280.f, 0.035f, 1.5f, 22.f, 0x5E5A0101u}},
           25.f);
    case Id::LiePass:
        // 测谎通过：约 1.5 秒胜利感上行 fanfare（明亮大调琶音 + 和弦叠置收尾；原 ~3s 压半）。
        return Synthesize({
            // 起势：G5 → B5 → D6
            {784.f, 0.f, 110.f, 0.28f, 6.f, 50.f},
            {988.f, 90.f, 120.f, 0.30f, 6.f, 55.f},
            {1175.f, 180.f, 130.f, 0.30f, 8.f, 60.f},
            // 中段抬升：G6 + 泛音
            {1568.f, 280.f, 160.f, 0.26f, 8.f, 70.f},
            {1960.f, 280.f, 140.f, 0.10f, 10.f, 65.f},
            // 二次上行：A5 → C6 → E6
            {880.f, 450.f, 120.f, 0.24f, 6.f, 55.f},
            {1047.f, 540.f, 130.f, 0.28f, 6.f, 60.f},
            {1319.f, 640.f, 150.f, 0.30f, 8.f, 70.f},
            // 胜利收束：C 大三和弦叠置 + 高八度叮（总长约 1.5s）
            {523.f, 825.f, 575.f, 0.22f, 10.f, 160.f},
            {659.f, 860.f, 575.f, 0.24f, 10.f, 160.f},
            {784.f, 900.f, 575.f, 0.26f, 10.f, 170.f},
            {1047.f, 940.f, 550.f, 0.20f, 12.f, 180.f},
            {2093.f, 1150.f, 325.f, 0.14f, 12.f, 200.f},
        }, {}, 30.f);
    case Id::GameContextOk:
        return Synthesize({
            {660.f, 0.f, 75.f, 0.28f, 4.f, 28.f},
            {990.f, 60.f, 110.f, 0.26f, 5.f, 45.f},
            {1320.f, 140.f, 160.f, 0.20f, 8.f, 70.f},
        });
    case Id::ScrollDrop:
        // 卷軸掉落：连续三声「叮→咚」（高短 + 低长），间隔清晰可数，比单段琶音更好听
        return Synthesize({
            // 第 1 声
            {1568.f, 0.f, 72.f, 0.36f, 3.f, 42.f},     // 叮 G6
            {1174.7f, 58.f, 100.f, 0.30f, 4.f, 58.f},   // 咚 D6
            // 第 2 声
            {1760.f, 290.f, 72.f, 0.34f, 3.f, 42.f},    // 叮 A6
            {1318.5f, 348.f, 100.f, 0.28f, 4.f, 58.f},  // 咚 E6
            // 第 3 声（略抬高收束）
            {1975.5f, 580.f, 72.f, 0.32f, 3.f, 42.f},   // 叮 B6
            {1480.f, 638.f, 120.f, 0.26f, 5.f, 75.f},   // 咚 F#6
        }, {}, 45.f);
    default:
        return Synthesize({{440.f, 0.f, 60.f, 0.25f, 3.f, 30.f}});
    }
}

}  // namespace xcat::sound::presets
