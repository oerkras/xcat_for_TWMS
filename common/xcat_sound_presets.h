#pragma once

#include "xcat_sound.h"

#include <cstdint>
#include <vector>

namespace xcat::sound::presets {

constexpr int kSampleRate = 44100;

struct Tone {
    float freqHz     = 440.f;
    float startMs    = 0.f;
    float durationMs = 80.f;
    float gain       = 0.35f;
    float attackMs   = 3.f;
    float releaseMs  = 40.f;
};

struct NoiseBurst {
    float startMs    = 0.f;
    float durationMs = 60.f;
    float gain       = 0.12f;
    float attackMs   = 1.f;
    float releaseMs  = 30.f;
    uint32_t seed    = 0xA5A5A5A5u;
};

std::vector<int16_t> Synthesize(const std::vector<Tone>& tones, const std::vector<NoiseBurst>& noise = {},
                                float tailPadMs = 20.f);

std::vector<int16_t> Generate(Id id);

}  // namespace xcat::sound::presets
